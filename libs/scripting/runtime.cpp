// The resource manager and event dispatcher: discovery, dependency order, start/stop/restart,
// timers, commands, cross-resource calls, and the entry points the server calls.
//
// Everything here is host code (not a lua_CFunction), but several bodies run inside a resource's
// protected call (Resource::Run) and follow the Lua error safety rules of internal.h.
#include "fsutil.h"
#include "internal.h"

#include <algorithm>
#include <cstring>

namespace gage::scripting {

    using detail::Engine;
    using detail::EntryScope;
    using detail::Resource;

    namespace {

        std::string Lower(const std::string& s)
        {
            std::string o = s;
            for (char& c : o)
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            return o;
        }

        std::string TrimStr(const std::string& s)
        {
            size_t a = 0, b = s.size();
            while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
            while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
            return s.substr(a, b - a);
        }

        // Splits on spaces and tabs; at most `maxTokens` tokens (the rest is dropped).
        std::vector<std::string> Split(const std::string& s, size_t maxTokens)
        {
            std::vector<std::string> out;
            size_t i = 0;
            while (i < s.size() && out.size() < maxTokens)
            {
                while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
                if (i >= s.size()) break;
                size_t j = i;
                while (j < s.size() && s[j] != ' ' && s[j] != '\t') j++;
                out.push_back(s.substr(i, j - i));
                i = j;
            }
            return out;
        }

        const char* LeaveName(LeaveReason r)
        {
            switch (r)
            {
            case LeaveReason::Quit: return "quit";
            case LeaveReason::Timeout: return "timeout";
            case LeaveReason::Kicked: return "kicked";
            }
            return "quit";
        }

        template <class T>
        T Clamp(T v, T lo, T hi)
        {
            return v < lo ? lo : (v > hi ? hi : v);
        }

        RuntimeOptions Clamped(RuntimeOptions o)
        {
            o.limits.instructionBudget = Clamp<uint64_t>(o.limits.instructionBudget, 1000, 1ull << 40);
            o.limits.timeBudgetMs = Clamp<uint32_t>(o.limits.timeBudgetMs, 1, 60000);
            o.limits.memoryBytes = Clamp<size_t>(o.limits.memoryBytes, 1u << 20, static_cast<size_t>(1) << 31);
            o.limits.patternSteps = Clamp<uint64_t>(o.limits.patternSteps, 1000, 4000000000ull);
            o.stateUpdateHz = Clamp<uint32_t>(o.stateUpdateHz, 1, 60);
            o.storageFlushMs = Clamp<uint32_t>(o.storageFlushMs, 100, 3600000);
            o.storageMaxBytes = Clamp<size_t>(o.storageMaxBytes, 1024, static_cast<size_t>(1) << 30);
            o.maxTimersPerResource = Clamp<uint32_t>(o.maxTimersPerResource, 1, 65535);
            o.maxHandlersPerResource = Clamp<uint32_t>(o.maxHandlersPerResource, 1, 65535);
            o.logBurst = Clamp<uint32_t>(o.logBurst, 1, 10000);
            o.logPerSecond = Clamp<uint32_t>(o.logPerSecond, 1, 100000);
            if (o.resourcesDir.empty()) o.resourcesDir = "resources";
            if (o.dataDir.empty()) o.dataDir = "data";
            return o;
        }

        // ---- region bodies (run inside a resource's protected call) ----------------------------

        struct HandlersCtx
        {
            Resource* r;
            const std::vector<uint64_t>* ids;
            const detail::ArgPusher* push;
            const std::string* what;
        };

        void HandlersBody(lua_State* L, void* ud)
        {
            HandlersCtx* c = static_cast<HandlersCtx*>(ud);
            Resource* r = c->r;
            for (size_t i = 0; i < c->ids->size(); i++)
            {
                auto it = r->handlers.find((*c->ids)[i]);
                if (it == r->handlers.end()) continue; // removed by an earlier handler of this event
                if (r->faulted) break;
                lua_rawgeti(L, LUA_REGISTRYINDEX, it->second.ref);
                const int n = (*c->push)(L);
                r->Invoke(n, 0, c->what->c_str(), nullptr);
            }
        }

        struct SingleCtx
        {
            Resource* r;
            int ref;
            bool release; // one-shot timer: free the registry slot as the call starts
            const detail::ArgPusher* push;
            const char* what;
            bool ok = true; // Invoke() succeeded (false: the handler raised an error, already logged)
        };

        void SingleBody(lua_State* L, void* ud)
        {
            SingleCtx* c = static_cast<SingleCtx*>(ud);
            lua_rawgeti(L, LUA_REGISTRYINDEX, c->ref);
            if (c->release) luaL_unref(L, LUA_REGISTRYINDEX, c->ref); // the function stays alive on the stack
            const int n = c->push ? (*c->push)(L) : 0;
            c->ok = c->r->Invoke(n, 0, c->what, nullptr);
        }

        struct ExportCtx
        {
            Resource* t;
            int ref;
            const std::vector<Value>* args;
            std::vector<Value>* results;
            std::string* err;
            const std::string* what;
            bool failed;
        };

        void ExportBody(lua_State* L, void* ud)
        {
            ExportCtx* c = static_cast<ExportCtx*>(ud);
            luaL_checkstack(L, static_cast<int>(c->args->size()) + 8, "too many arguments");
            const int base = lua_gettop(L);
            lua_rawgeti(L, LUA_REGISTRYINDEX, c->ref);
            for (const Value& v : *c->args) ToLua(L, v);
            if (!c->t->Invoke(static_cast<int>(c->args->size()), LUA_MULTRET, c->what->c_str(), c->err))
            {
                c->failed = true;
                return;
            }
            const int nres = lua_gettop(L) - base;
            if (nres > 32)
            {
                *c->err = "the export returned too many values (limit 32)";
                c->failed = true;
                lua_settop(L, base);
                return;
            }
            std::string cerr;
            for (int i = 1; i <= nres; i++)
            {
                c->results->emplace_back();
                if (!FromLua(L, base + i, c->results->back(), &cerr))
                {
                    *c->err = "return value #" + std::to_string(i) + ": " + cerr;
                    c->failed = true;
                    break;
                }
            }
            lua_settop(L, base);
        }

        struct LoadCtx
        {
            Resource* r;
            const std::string* code;
            const char* chunk;
            const char* what;
            std::string* err;
            bool ok;
        };

        void LoadBody(lua_State* L, void* ud)
        {
            LoadCtx* c = static_cast<LoadCtx*>(ud);
            const int st = luaL_loadbufferx(L, c->code->data(), c->code->size(), c->chunk, "t");
            if (st != LUA_OK)
            {
                const char* m = lua_tostring(L, -1);
                *c->err = m ? m : "(compile error)";
                lua_pop(L, 1);
                c->ok = false;
                return;
            }
            c->ok = c->r->Invoke(0, 0, c->what, c->err);
        }

    }

    namespace detail {

        // ---- construction and logging ---------------------------------------------------------------

        Engine::Engine(IServerApi& a, const RuntimeOptions& options) : api(a), opt(Clamped(options)) {}

        Engine::~Engine()
        {
            Shutdown();
        }

        void Engine::LogSys(LogLevel level, const std::string& text)
        {
            api.Log(level, "script: " + text);
        }

        void Engine::LogScript(Resource& r, LogLevel level, const std::string& text)
        {
            const uint64_t now = api.NowMs();
            const double perMs = static_cast<double>(opt.logPerSecond) / 1000.0;
            if (now > r.logRefillMs)
            {
                r.logTokens = std::min<double>(static_cast<double>(opt.logBurst), r.logTokens + static_cast<double>(now - r.logRefillMs) * perMs);
                r.logRefillMs = now;
            }
            if (r.logTokens < 1.0)
            {
                r.logSuppressed++;
                return;
            }
            r.logTokens -= 1.0;
            if (r.logSuppressed)
            {
                api.Log(LogLevel::Warn, "[" + r.name + "] " + std::to_string(r.logSuppressed) + " log lines suppressed (rate limit)");
                r.logSuppressed = 0;
            }
            api.Log(level, "[" + r.name + "] " + fsutil::SanitizeForLog(fsutil::Utf8Truncate(text, 8000)));
        }

        // ---- lookup ---------------------------------------------------------------------------------

        Resource* Engine::FindRunning(const std::string& name) const
        {
            for (const auto& up : running)
                if (up->name == name) return up.get();
            return nullptr;
        }

        bool Engine::IsLive(const Resource* r) const
        {
            for (const auto& up : running)
                if (up.get() == r) return true;
            return false;
        }

        bool Engine::IsRunning(const std::string& name) const
        {
            return FindRunning(name) != nullptr;
        }

        // ---- discovery ------------------------------------------------------------------------------

        // Reads resources/<name>/resource.json (and the optional config.json) into known[name].
        // A running resource keeps its manifest. See internal.h for what a failure leaves behind.
        bool Engine::LoadKnown(const std::string& name)
        {
            if (Resource* r = FindRunning(name))
            {
                Known& k = known[name];
                k.manifest = r->manifest;
                k.valid = true;
                return true;
            }
            if (!fsutil::ValidResourceName(name))
            {
                known.erase(name);
                lastLoadError = "not a valid resource name";
                return false;
            }
            const std::string dir = opt.resourcesDir + "/" + name;
            std::string text, why;
            bool missing = false;
            const bool readOk = fsutil::ReadFile(dir + "/resource.json", text, &why, &missing, 1024 * 1024);
            if (!readOk && missing)
            {
                known.erase(name);
                lastLoadError = "resource.json not found";
                return false;
            }
            Known& k = known[name];
            k.valid = false;
            k.error.clear();
            k.startFailed = false;
            k.blocked = false;
            k.blockedReason.clear();
            auto invalid = [&](const std::string& reason) {
                k.error = reason;
                lastLoadError = reason;
                return false;
            };
            if (!readOk) return invalid("cannot read resource.json: " + why);
            Manifest m;
            if (!ParseManifest(text, name, m, &why)) return invalid(why);
            std::string cfg;
            bool cfgMissing = false;
            std::string cfgWhy;
            if (fsutil::ReadFile(dir + "/config.json", cfg, &cfgWhy, &cfgMissing, 1024 * 1024))
            {
                if (!ApplyConfigOverride(cfg, m, &why)) return invalid(why);
            }
            else if (!cfgMissing)
                return invalid("cannot read config.json: " + cfgWhy);
            k.manifest = std::move(m);
            k.valid = true;
            return true;
        }

        size_t Engine::Refresh()
        {
            std::vector<std::string> names;
            std::string why;
            if (!fsutil::ListSubdirs(opt.resourcesDir, names, &why)) LogSys(LogLevel::Error, "cannot scan resources: " + why);
            std::set<std::string> seen;
            size_t valid = 0;
            for (const std::string& name : names)
            {
                if (!fsutil::ValidResourceName(name)) continue;
                if (!fsutil::IsRegularFile(opt.resourcesDir + "/" + name + "/resource.json")) continue; // not a resource
                seen.insert(name);
                const std::string before = known.count(name) ? known[name].error : std::string();
                if (LoadKnown(name))
                    valid++;
                else if (known.count(name) && known[name].error != before)
                    LogSys(LogLevel::Error, "resource '" + name + "' ignored: " + known[name].error);
            }
            for (auto it = known.begin(); it != known.end();)
            {
                if (!seen.count(it->first) && !FindRunning(it->first))
                    it = known.erase(it);
                else
                    ++it;
            }
            return valid;
        }

        // Orders `roots` and their dependencies so that dependencies come first. Resources that
        // cannot start (cycle, missing or invalid dependency, a blocked dependency) are left out
        // of `order` and reported in `blocked` with the reason.
        bool Engine::PlanStart(const std::vector<std::string>& roots, std::vector<std::string>& order,
                               std::map<std::string, std::string>& blocked)
        {
            enum Color { White, Gray, Black };
            std::map<std::string, Color> color;
            std::vector<std::string> stack;
            std::function<void(const std::string&)> visit = [&](const std::string& name) {
                color[name] = Gray;
                stack.push_back(name);
                const std::vector<std::string> deps = known[name].manifest.dependencies;
                for (const std::string& dep : deps)
                {
                    if (!color.count(dep))
                    {
                        if (!LoadKnown(dep))
                        {
                            blocked.emplace(name, "dependency '" + dep + "' cannot be used: " + lastLoadError);
                            continue;
                        }
                    }
                    if (color[dep] == Gray)
                    {
                        std::string path;
                        size_t from = 0;
                        for (size_t i = 0; i < stack.size(); i++)
                            if (stack[i] == dep) from = i;
                        for (size_t i = from; i < stack.size(); i++) path += stack[i] + " -> ";
                        path += dep;
                        for (size_t i = from; i < stack.size(); i++) blocked.emplace(stack[i], "dependency cycle: " + path);
                        continue;
                    }
                    if (color[dep] == White || !color.count(dep)) visit(dep);
                    auto b = blocked.find(dep);
                    if (b != blocked.end()) blocked.emplace(name, "depends on '" + dep + "' which cannot start (" + b->second + ")");
                }
                stack.pop_back();
                color[name] = Black;
                if (!blocked.count(name)) order.push_back(name);
            };
            for (const std::string& root : roots)
                if (!color.count(root)) visit(root);
            return true;
        }

        // ---- lifecycle ------------------------------------------------------------------------------

        bool Engine::LoadScripts(Resource& r, std::string* err)
        {
            for (const std::string& rel : r.manifest.serverScripts)
            {
                std::string path, why;
                if (!fsutil::ResolveInside(r.dir, rel, &path, &why))
                {
                    *err = rel + ": " + why;
                    return false;
                }
                std::string code;
                if (!fsutil::ReadFile(path, code, &why, nullptr, kMaxScriptBytes))
                {
                    *err = rel + ": cannot read: " + why;
                    return false;
                }
                code = fsutil::StripBom(code);
                const std::string chunk = "@" + r.name + "/" + rel;
                const std::string what = "script " + r.name + "/" + rel;
                std::string runErr;
                LoadCtx ctx{&r, &code, chunk.c_str(), what.c_str(), &runErr, false};
                std::string protectedErr;
                if (!r.Run(&LoadBody, &ctx, &protectedErr))
                {
                    *err = rel + ": " + FirstPart(protectedErr);
                    return false;
                }
                if (!ctx.ok)
                {
                    *err = rel + ": " + runErr;
                    return false;
                }
            }
            return true;
        }

        // Removes what the resource registered outside its own state: pending timers and command
        // names. (Handlers and exports live in the state and go with it.)
        void Engine::Unregister(Resource& r)
        {
            for (const auto& kv : r.timers) timerQueue.erase(TimerKey{kv.second.due, kv.second.seq, &r, kv.first});
            r.timers.clear();
            for (const auto& kv : r.commands) commandOwner.erase(kv.first);
            r.commands.clear();
            r.handlers.clear();
            r.handlerOrder.clear();
            r.exportRefs.clear();
            pendingStops.erase(std::remove(pendingStops.begin(), pendingStops.end(), r.name), pendingStops.end());
        }

        bool Engine::StartOne(const std::string& name, std::string* err)
        {
            Known& k = known[name];
            auto owned = std::make_unique<Resource>();
            Resource& r = *owned;
            r.rt = this;
            r.manifest = k.manifest;
            r.name = name;
            r.dir = opt.resourcesDir + "/" + name;
            r.dataDir = opt.dataDir + "/" + name;
            r.startSeq = nextStartSeq++;
            r.logTokens = static_cast<double>(opt.logBurst);
            r.logRefillMs = api.NowMs();

            auto fail = [&](const std::string& why) {
                Unregister(r);
                CloseState(r);
                Known& kk = known[name];
                kk.startFailed = true;
                kk.error = why;
                LogSys(LogLevel::Error, "resource '" + name + "' failed to start: " + why);
                if (err) *err = why;
                return false;
            };

            std::string why;
            AtomicWriteFn writer = opt.atomicWrite;
            if (!writer)
                writer = [](const std::string& p, const std::string& c, std::string* e) { return fsutil::AtomicWrite(p, c, e); };
            if (!r.storage.Open(r.dataDir, writer, opt.storageMaxBytes, &why)) return fail(why);
            if (!CreateState(r, &why)) return fail(why);
            if (!LoadScripts(r, &why)) return fail(why);
            if (r.faulted) return fail("aborted for exceeding its budget while loading");

            Resource* raw = owned.get();
            running.push_back(std::move(owned));
            known[name].startFailed = false;
            known[name].error.clear();
            known[name].blocked = false;
            LogSys(LogLevel::Info, "resource '" + name + "' started (version " + raw->manifest.version + ")");
            const std::string started = name;
            DispatchAll("resourceStart",
                        [&started](lua_State* L) -> int {
                            lua_pushlstring(L, started.data(), started.size());
                            return 1;
                        },
                        false);
            return true;
        }

        bool Engine::Start(const std::string& name, std::string* err)
        {
            if (!fsutil::ValidResourceName(name))
            {
                if (err) *err = "\"" + name + "\" is not a valid resource name";
                return false;
            }
            if (FindRunning(name)) return true;
            if (!LoadKnown(name))
            {
                if (err) *err = "resource '" + name + "': " + lastLoadError;
                return false;
            }
            std::vector<std::string> order;
            std::map<std::string, std::string> blocked;
            PlanStart({name}, order, blocked);
            for (const auto& b : blocked)
            {
                if (known.count(b.first))
                {
                    known[b.first].blocked = true;
                    known[b.first].blockedReason = b.second;
                }
            }
            if (blocked.count(name))
            {
                if (err) *err = "resource '" + name + "' cannot start: " + blocked[name];
                return false;
            }
            for (const std::string& n : order)
            {
                if (FindRunning(n)) continue;
                std::string e;
                if (!StartOne(n, &e))
                {
                    if (err) *err = n == name ? e : "dependency '" + n + "' failed to start: " + e;
                    return false;
                }
            }
            return FindRunning(name) != nullptr;
        }

        size_t Engine::StartAll()
        {
            Refresh();
            std::vector<std::string> roots;
            for (const auto& kv : known)
                if (kv.second.valid) roots.push_back(kv.first);
            std::vector<std::string> order;
            std::map<std::string, std::string> blocked;
            PlanStart(roots, order, blocked);
            for (const auto& b : blocked)
            {
                if (!known.count(b.first)) continue;
                known[b.first].blocked = true;
                known[b.first].blockedReason = b.second;
                LogSys(LogLevel::Error, "resource '" + b.first + "' not started: " + b.second);
            }
            std::set<std::string> failed;
            for (const std::string& n : order)
            {
                if (FindRunning(n)) continue;
                bool depFailed = false;
                for (const std::string& d : known[n].manifest.dependencies)
                    if (failed.count(d)) depFailed = true;
                if (depFailed)
                {
                    failed.insert(n);
                    known[n].blocked = true;
                    known[n].blockedReason = "a dependency failed to start";
                    LogSys(LogLevel::Error, "resource '" + n + "' not started: a dependency failed to start");
                    continue;
                }
                std::string e;
                if (!StartOne(n, &e)) failed.insert(n);
            }
            return running.size();
        }

        // The running resources that depend (directly or not) on `name`, newest first (the order
        // in which they must stop).
        std::vector<std::string> Engine::DependentsOf(const std::string& name) const
        {
            std::set<std::string> closure;
            closure.insert(name);
            for (bool changed = true; changed;)
            {
                changed = false;
                for (const auto& up : running)
                {
                    if (closure.count(up->name)) continue;
                    for (const std::string& d : up->manifest.dependencies)
                        if (closure.count(d))
                        {
                            closure.insert(up->name);
                            changed = true;
                            break;
                        }
                }
            }
            closure.erase(name);
            std::vector<std::string> out;
            for (auto it = running.rbegin(); it != running.rend(); ++it)
                if (closure.count((*it)->name)) out.push_back((*it)->name);
            return out;
        }

        void Engine::Teardown(Resource& target, bool deliverStop)
        {
            Resource* r = &target;
            const std::string name = r->name;
            if (deliverStop)
            {
                DispatchAll("resourceStop",
                            [&name](lua_State* L) -> int {
                                lua_pushlstring(L, name.data(), name.size());
                                return 1;
                            },
                            false);
            }
            std::string why;
            if (!r->storage.Flush(&why)) LogSys(LogLevel::Error, "resource '" + name + "': storage not saved: " + why);
            Unregister(*r);
            std::unique_ptr<Resource> owned;
            for (auto it = running.begin(); it != running.end(); ++it)
                if (it->get() == r)
                {
                    owned = std::move(*it);
                    running.erase(it);
                    break;
                }
            CloseState(*r);
            LogSys(LogLevel::Info, "resource '" + name + "' stopped");
        }

        void Engine::StopClosure(const std::string& name, std::vector<std::string>* stoppedDependents)
        {
            for (const std::string& d : DependentsOf(name))
            {
                Resource* dr = FindRunning(d);
                if (!dr) continue;
                LogSys(LogLevel::Info, "stopping '" + d + "' because it depends on '" + name + "'");
                Teardown(*dr, true);
                if (stoppedDependents) stoppedDependents->push_back(d);
            }
            if (Resource* r = FindRunning(name)) Teardown(*r, true);
        }

        bool Engine::Stop(const std::string& name, std::string* err)
        {
            if (!FindRunning(name))
            {
                if (err) *err = "resource '" + name + "' is not running";
                return false;
            }
            StopClosure(name, nullptr);
            return true;
        }

        bool Engine::Restart(const std::string& name, std::string* err)
        {
            std::vector<std::string> dependents;
            if (FindRunning(name)) StopClosure(name, &dependents);
            if (!Start(name, err)) return false;
            // The dependents were stopped only because `name` went down: bring them back in start order.
            for (auto it = dependents.rbegin(); it != dependents.rend(); ++it)
            {
                std::string e;
                if (!Start(*it, &e)) LogSys(LogLevel::Error, "could not restart '" + *it + "' after '" + name + "': " + e);
            }
            return true;
        }

        void Engine::Shutdown()
        {
            ++depth;
            while (!running.empty()) Teardown(*running.back(), true);
            --depth;
            pendingStops.clear();
            stateTracks.clear();
            timerQueue.clear();
            commandOwner.clear();
        }

        void Engine::FlushDeferred()
        {
            if (flushing) return;
            flushing = true;
            ++depth;
            while (!pendingStops.empty())
            {
                const std::string n = pendingStops.front();
                pendingStops.erase(pendingStops.begin());
                if (!FindRunning(n)) continue;
                LogSys(LogLevel::Warn, "stopping resource '" + n + "' after repeated budget violations");
                StopClosure(n, nullptr);
                if (known.count(n))
                {
                    known[n].startFailed = true;
                    known[n].error = "stopped after repeated budget violations";
                }
            }
            --depth;
            flushing = false;
        }

        // ---- status ---------------------------------------------------------------------------------

        std::vector<ResourceStatus> Engine::Resources() const
        {
            std::vector<ResourceStatus> out;
            for (const auto& kv : known)
            {
                ResourceStatus s;
                s.name = kv.first;
                s.version = kv.second.valid ? kv.second.manifest.version : std::string();
                s.dependencies = kv.second.valid ? kv.second.manifest.dependencies : std::vector<std::string>();
                if (Resource* r = FindRunning(kv.first))
                {
                    s.state = ResourceState::Running;
                    s.version = r->manifest.version;
                    s.memoryBytes = r->mem.used;
                    s.handlers = r->handlers.size();
                    s.timers = r->timers.size();
                    s.commands = r->commands.size();
                    s.exports = r->exportRefs.size();
                }
                else if (!kv.second.valid || kv.second.startFailed)
                {
                    s.state = ResourceState::Failed;
                    s.detail = kv.second.error;
                }
                else if (kv.second.blocked)
                {
                    s.state = ResourceState::Blocked;
                    s.detail = kv.second.blockedReason;
                }
                out.push_back(std::move(s));
            }
            return out; // known is a std::map: already sorted by name
        }

        RuntimeStats Engine::Stats() const
        {
            RuntimeStats s;
            s.running = running.size();
            for (const auto& up : running)
            {
                s.handlers += up->handlers.size();
                s.luaBytes += up->mem.used;
            }
            s.timers = timerQueue.size();
            s.commands = commandOwner.size();
            return s;
        }

        std::string Engine::ListResources() const
        {
            const std::vector<ResourceStatus> all = Resources();
            size_t up = 0;
            for (const ResourceStatus& s : all)
                if (s.state == ResourceState::Running) up++;
            std::string out = "resources " + std::to_string(all.size()) + " (" + std::to_string(up) + " running)";
            for (const ResourceStatus& s : all)
            {
                const char* st = s.state == ResourceState::Running   ? "started"
                                 : s.state == ResourceState::Stopped ? "stopped"
                                 : s.state == ResourceState::Failed  ? "failed"
                                                                     : "blocked";
                out += "\n  " + s.name + "  " + (s.version.empty() ? std::string("-") : s.version) + "  " + st;
                if (!s.detail.empty()) out += ": " + s.detail;
            }
            return out;
        }

        // ---- dispatch -------------------------------------------------------------------------------

        bool Engine::DispatchTo(Resource& r, const std::string& event, const ArgPusher& push, bool cancellable)
        {
            if (r.faulted || !r.L) return false;
            auto oit = r.handlerOrder.find(event);
            if (oit == r.handlerOrder.end()) return false;
            const std::vector<uint64_t> ids = oit->second;
            const std::string what = "handler for '" + event + "'";
            const bool prevCancellable = r.cancellable, prevCancelled = r.cancelled;
            r.cancellable = cancellable;
            r.cancelled = false;
            HandlersCtx c{&r, &ids, &push, &what};
            std::string err;
            if (!r.Run(&HandlersBody, &c, &err))
                LogScript(r, LogLevel::Error, "event '" + event + "' could not be delivered: " + FirstPart(err));
            const bool cancelled = r.cancelled;
            r.cancellable = prevCancellable;
            r.cancelled = prevCancelled;
            return cancelled;
        }

        bool Engine::DispatchAll(const std::string& event, const ArgPusher& push, bool cancellable)
        {
            std::vector<Resource*> targets;
            targets.reserve(running.size());
            for (const auto& up : running) targets.push_back(up.get());
            bool cancelled = false;
            for (Resource* t : targets)
            {
                if (!IsLive(t)) continue;
                if (DispatchTo(*t, event, push, cancellable)) cancelled = true;
            }
            return cancelled;
        }

        bool Engine::EmitCustom(Resource& from, const std::string& event, const std::vector<Value>& args, std::string* err)
        {
            (void)from;
            if (callDepth >= kMaxCallDepth)
            {
                if (err) *err = "events are nested too deeply (limit " + std::to_string(kMaxCallDepth) + ")";
                return false;
            }
            callDepth++;
            const ArgPusher push = [&args](lua_State* L) -> int {
                luaL_checkstack(L, static_cast<int>(args.size()) + 4, "too many event arguments");
                for (const Value& v : args) ToLua(L, v);
                return static_cast<int>(args.size());
            };
            DispatchAll(event, push, false);
            callDepth--;
            return true;
        }

        bool Engine::CallExport(const std::string& target, const std::string& fn, const std::vector<Value>& args,
                                std::vector<Value>& results, std::string* err)
        {
            Resource* t = FindRunning(target);
            if (!t || t->faulted)
            {
                *err = "resource '" + target + "' is not running";
                return false;
            }
            auto it = t->exportRefs.find(fn);
            if (it == t->exportRefs.end())
            {
                *err = "resource '" + target + "' has no export '" + fn + "'";
                return false;
            }
            if (callDepth >= kMaxCallDepth)
            {
                *err = "exports are nested too deeply (limit " + std::to_string(kMaxCallDepth) + ")";
                return false;
            }
            const std::string what = "export '" + fn + "'";
            std::string callErr, runErr;
            ExportCtx ctx{t, it->second, &args, &results, &callErr, &what, false};
            callDepth++;
            const bool ran = t->Run(&ExportBody, &ctx, &runErr);
            callDepth--;
            if (!ran)
            {
                *err = FirstPart(runErr);
                return false;
            }
            if (ctx.failed)
            {
                *err = callErr;
                return false;
            }
            return true;
        }

        // ---- timers ---------------------------------------------------------------------------------

        uint32_t Engine::AddTimer(Resource& r, int ref, uint32_t delayMs, bool repeat)
        {
            const uint32_t id = r.nextTimerId++;
            TimerRec t;
            t.ref = ref;
            t.repeat = repeat;
            t.intervalMs = delayMs;
            t.due = api.NowMs() + delayMs;
            t.seq = nextSeq++;
            r.timers.emplace(id, t);
            timerQueue.insert(TimerKey{t.due, t.seq, &r, id});
            return id;
        }

        bool Engine::ClearTimer(Resource& r, uint32_t id, int* refOut)
        {
            auto it = r.timers.find(id);
            if (it == r.timers.end()) return false;
            timerQueue.erase(TimerKey{it->second.due, it->second.seq, &r, id});
            if (refOut) *refOut = it->second.ref;
            r.timers.erase(it);
            return true;
        }

        void Engine::FireDueTimers()
        {
            const uint64_t now = api.NowMs();
            const uint64_t seqLimit = nextSeq; // timers created from here on wait for the next tick
            for (;;)
            {
                auto it = timerQueue.begin();
                while (it != timerQueue.end() && it->due <= now && it->seq >= seqLimit) ++it;
                if (it == timerQueue.end() || it->due > now) break;
                const TimerKey key = *it;
                timerQueue.erase(it);
                Resource* r = key.res;
                auto tit = r->timers.find(key.id);
                if (tit == r->timers.end()) continue;
                const int ref = tit->second.ref;
                const bool repeat = tit->second.repeat;
                if (repeat)
                {
                    uint64_t next = key.due + tit->second.intervalMs;
                    if (next <= now) next = now + tit->second.intervalMs; // never a burst of catch-up calls
                    tit->second.due = next;
                    tit->second.seq = nextSeq++;
                    timerQueue.insert(TimerKey{next, tit->second.seq, r, key.id});
                }
                else
                    r->timers.erase(tit); // before the call: the callback may create or clear timers
                if (r->faulted) continue;
                const std::string what = "timer #" + std::to_string(key.id);
                SingleCtx ctx{r, ref, !repeat, nullptr, what.c_str()};
                std::string err;
                if (!r->Run(&SingleBody, &ctx, &err))
                    LogScript(*r, LogLevel::Error, "timer #" + std::to_string(key.id) + " could not run: " + FirstPart(err));
            }
        }

        // ---- commands -------------------------------------------------------------------------------

        bool Engine::IsReservedCommand(const std::string& name) const
        {
            static const char* const kReserved[] = {"list", "kick", "say",   "ban",   "banip",     "unban",   "bans",
                                                    "help", "quit", "exit",  "stop",  "start",     "restart", "refresh",
                                                    "resources", "stats"};
            for (const char* k : kReserved)
                if (name == k) return true;
            return false;
        }

        bool Engine::RegisterCommand(Resource& r, const std::string& name, int ref, bool restricted, const std::string& help,
                                     std::string* err)
        {
            if (IsReservedCommand(name))
            {
                *err = "'" + name + "' is a built-in server command";
                return false;
            }
            auto own = commandOwner.find(name);
            if (own != commandOwner.end() && own->second != r.name)
            {
                *err = "command '" + name + "' is already registered by resource '" + own->second + "'";
                return false;
            }
            auto it = r.commands.find(name);
            if (it == r.commands.end() && r.commands.size() >= 128)
            {
                *err = "too many commands (limit 128)";
                return false;
            }
            if (it != r.commands.end())
            {
                luaL_unref(r.L, LUA_REGISTRYINDEX, it->second.ref); // replaced: release the old handler
                it->second.ref = ref;
                it->second.restricted = restricted;
                it->second.help = help;
            }
            else
            {
                CommandRec c;
                c.ref = ref;
                c.restricted = restricted;
                c.help = help;
                r.commands.emplace(name, std::move(c));
            }
            commandOwner[name] = r.name;
            return true;
        }

        void Engine::ConsoleReply(const std::string& text)
        {
            if (consoleOut)
            {
                *consoleOut += text;
                *consoleOut += "\n";
            }
            else
                LogSys(LogLevel::Info, text);
        }

        bool Engine::RunCommand(const std::string& name, const Source& src, const std::vector<std::string>& args, const std::string& raw)
        {
            auto own = commandOwner.find(name);
            Resource* r = own == commandOwner.end() ? nullptr : FindRunning(own->second);
            auto reply = [&](const std::string& text) {
                if (src.console)
                    ConsoleReply(text);
                else
                    api.SendChat(src.player.id, text, nullptr);
            };
            if (!r || r->faulted || !r->commands.count(name))
            {
                reply("Command /" + name + " is not available.");
                return false;
            }
            const CommandRec& c = r->commands[name];
            const bool isAdmin = src.console || src.player.isAdmin;
            if (c.restricted && !isAdmin)
            {
                reply("You do not have permission to use /" + name + ".");
                LogSys(LogLevel::Info, "player " + src.player.name + " (id " + std::to_string(src.player.id) + ") was refused restricted command /" + name);
                return false;
            }
            const ArgPusher push = [&](lua_State* L) -> int {
                PushSource(L, src);
                lua_createtable(L, static_cast<int>(args.size()), 0);
                for (size_t i = 0; i < args.size(); i++)
                {
                    lua_pushlstring(L, args[i].data(), args[i].size());
                    lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
                }
                lua_pushlstring(L, raw.data(), raw.size());
                return 3;
            };
            const std::string what = "command /" + name;
            SingleCtx ctx{r, c.ref, false, &push, what.c_str()};
            std::string err;
            if (!r->Run(&SingleBody, &ctx, &err))
            {
                LogScript(*r, LogLevel::Error, what + " could not run: " + FirstPart(err));
                reply("The command failed.");
                return false;
            }
            if (!ctx.ok)
            {
                reply("The command failed (see the server log).");
                return false;
            }
            return true;
        }

        // ---- events from the server ---------------------------------------------------------------

        void Engine::Tick()
        {
            FireDueTimers();
            DeliverStateUpdates();
            FlushStorages(false);
        }

        void Engine::FlushStorages(bool all)
        {
            const uint64_t now = api.NowMs();
            std::vector<Resource*> targets;
            for (const auto& up : running) targets.push_back(up.get());
            for (Resource* r : targets)
            {
                if (!IsLive(r) || !r->storage.dirty()) continue;
                if (r->storageDirtySinceMs == 0) r->storageDirtySinceMs = now | 1;
                if (!all && now - r->storageDirtySinceMs < opt.storageFlushMs) continue;
                std::string why;
                if (r->storage.Flush(&why))
                    r->storageDirtySinceMs = 0;
                else
                {
                    LogSys(LogLevel::Error, "resource '" + r->name + "': storage not saved: " + why + " (will retry)");
                    r->storageDirtySinceMs = now | 1; // retry after another flush interval, not every tick
                }
            }
        }

        bool Engine::WantsPlayerState() const
        {
            for (const auto& up : running)
                if (!up->faulted && up->handlerOrder.count("playerStateUpdate")) return true;
            return false;
        }

        void Engine::OnPlayerState(uint16_t id, const PlayerState& s)
        {
            if (!WantsPlayerState()) return;
            StateTrack& t = stateTracks[id];
            t.state = s;
            t.dirty = true;
        }

        void Engine::DeliverStateUpdates()
        {
            if (stateTracks.empty()) return;
            const uint64_t now = api.NowMs();
            const uint64_t interval = 1000u / opt.stateUpdateHz;
            std::vector<uint16_t> ids;
            for (const auto& kv : stateTracks)
                if (kv.second.dirty) ids.push_back(kv.first);
            for (uint16_t id : ids)
            {
                auto it = stateTracks.find(id);
                if (it == stateTracks.end() || !it->second.dirty) continue;
                if (it->second.everDelivered && now - it->second.lastDeliveredMs < interval) continue;
                PlayerInfo p;
                if (!api.GetPlayer(id, p))
                {
                    stateTracks.erase(it);
                    continue;
                }
                const PlayerState st = it->second.state;
                it->second.dirty = false;
                it->second.everDelivered = true;
                it->second.lastDeliveredMs = now;
                const ArgPusher push = [&p, &st](lua_State* L) -> int {
                    PushPlayerTable(L, p);
                    lua_createtable(L, 0, 7);
                    lua_pushnumber(L, st.x);
                    lua_setfield(L, -2, "x");
                    lua_pushnumber(L, st.y);
                    lua_setfield(L, -2, "y");
                    lua_pushnumber(L, st.z);
                    lua_setfield(L, -2, "z");
                    lua_pushnumber(L, st.heading);
                    lua_setfield(L, -2, "heading");
                    lua_pushnumber(L, st.vx);
                    lua_setfield(L, -2, "vx");
                    lua_pushnumber(L, st.vy);
                    lua_setfield(L, -2, "vy");
                    lua_pushnumber(L, st.vz);
                    lua_setfield(L, -2, "vz");
                    return 2;
                };
                DispatchAll("playerStateUpdate", push, false);
            }
        }

        ConnectVerdict Engine::OnPlayerConnecting(const ConnectInfo& info)
        {
            ConnectVerdict v;
            if (running.empty()) return v;
            ConnectVerdict* previous = activeVerdict;
            activeVerdict = &v;
            const ArgPusher push = [&info](lua_State* L) -> int {
                lua_createtable(L, 0, 3);
                lua_pushlstring(L, info.name.data(), info.name.size());
                lua_setfield(L, -2, "name");
                lua_pushlstring(L, info.address.data(), info.address.size());
                lua_setfield(L, -2, "ip");
                lua_pushlstring(L, info.accountId.data(), info.accountId.size());
                lua_setfield(L, -2, "accountId");
                PushDeferral(L);
                return 2;
            };
            DispatchAll("playerConnecting", push, false);
            activeVerdict = previous;
            return v;
        }

        void Engine::OnPlayerJoined(uint16_t id)
        {
            PlayerInfo p;
            if (!api.GetPlayer(id, p)) return;
            DispatchAll("playerJoined",
                        [&p](lua_State* L) -> int {
                            PushPlayerTable(L, p);
                            return 1;
                        },
                        false);
        }

        void Engine::OnPlayerLeft(const PlayerInfo& info, LeaveReason why)
        {
            stateTracks.erase(info.id);
            const char* reason = LeaveName(why);
            DispatchAll("playerLeft",
                        [&info, reason](lua_State* L) -> int {
                            PushPlayerTable(L, info);
                            lua_pushstring(L, reason);
                            return 2;
                        },
                        false);
        }

        bool Engine::OnChatMessage(uint16_t playerId, const std::string& text)
        {
            if (running.empty()) return true;
            PlayerInfo p;
            if (!api.GetPlayer(playerId, p)) return true;
            if (!text.empty() && text[0] == '/')
            {
                const std::vector<std::string> tokens = Split(text, 33);
                const std::string name = Lower(tokens.empty() ? std::string() : tokens[0].substr(1));
                if (name.empty() || !commandOwner.count(name))
                {
                    api.SendChat(p.id, "Unknown command: /" + name + ".", nullptr);
                    return false;
                }
                std::vector<std::string> args(tokens.begin() + 1, tokens.end());
                size_t rawStart = text.find_first_of(" \t");
                const std::string raw = rawStart == std::string::npos ? std::string() : TrimStr(text.substr(rawStart));
                Source src;
                src.console = false;
                src.player = p;
                RunCommand(name, src, args, raw);
                return false;
            }
            const std::string chat = text;
            const bool cancelled = DispatchAll("chatMessage",
                                               [&p, &chat](lua_State* L) -> int {
                                                   PushPlayerTable(L, p);
                                                   lua_pushlstring(L, chat.data(), chat.size());
                                                   return 2;
                                               },
                                               true);
            return !cancelled;
        }

        ConsoleResult Engine::ExecuteConsole(const std::string& rawLine, std::string& output)
        {
            output.clear();
            const std::string line = TrimStr(rawLine);
            if (line.empty()) return ConsoleResult::NotHandled;
            const size_t sp = line.find_first_of(" \t");
            const std::string cmd = Lower(line.substr(0, sp));
            const std::string arg = sp == std::string::npos ? std::string() : TrimStr(line.substr(sp));

            if (cmd == "start" || cmd == "restart" || (cmd == "stop" && !arg.empty()))
            {
                if (arg.empty())
                {
                    output = "usage: " + cmd + " <resource>";
                    return ConsoleResult::Error;
                }
                std::string err;
                bool ok;
                if (cmd == "start")
                    ok = Start(arg, &err);
                else if (cmd == "stop")
                    ok = Stop(arg, &err);
                else
                    ok = Restart(arg, &err);
                if (!ok)
                {
                    output = err;
                    return ConsoleResult::Error;
                }
                output = (cmd == "start" ? "started " : cmd == "stop" ? "stopped " : "restarted ") + arg;
                return ConsoleResult::Ok;
            }
            if (cmd == "stop") return ConsoleResult::NotHandled; // the server's own `stop` quits
            if (cmd == "refresh")
            {
                const size_t n = Refresh();
                output = "found " + std::to_string(n) + " valid resource" + (n == 1 ? "" : "s");
                for (const ResourceStatus& s : Resources())
                    if (s.state == ResourceState::Failed && !s.detail.empty() && !IsRunning(s.name))
                        output += "\n  " + s.name + ": " + s.detail;
                return ConsoleResult::Ok;
            }
            if (cmd == "resources")
            {
                output = ListResources();
                return ConsoleResult::Ok;
            }
            if (!commandOwner.count(cmd)) return ConsoleResult::NotHandled;

            Source src;
            src.console = true;
            std::string* previous = consoleOut;
            consoleOut = &output;
            const bool ok = RunCommand(cmd, src, Split(arg, 32), arg);
            consoleOut = previous;
            while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
            if (!ok && output.empty()) output = "the command failed (see the server log)";
            return ok ? ConsoleResult::Ok : ConsoleResult::Error;
        }

    }

    // ---- ScriptRuntime (the public face) -------------------------------------------------------

    ScriptRuntime::ScriptRuntime(IServerApi& api, const RuntimeOptions& options) : impl_(new detail::Engine(api, options)) {}

    ScriptRuntime::~ScriptRuntime()
    {
        if (impl_) impl_->Shutdown();
    }

    bool ScriptRuntime::Validate(const RuntimeOptions& o, std::string* err)
    {
        auto bad = [&](const std::string& why) {
            if (err) *err = "scripting: " + why;
            return false;
        };
        if (o.resourcesDir.empty()) return bad("resourcesDir must not be empty");
        if (o.dataDir.empty()) return bad("dataDir must not be empty");
        if (o.limits.instructionBudget < 1000) return bad("limits.instructionBudget must be at least 1000");
        if (o.limits.timeBudgetMs < 1 || o.limits.timeBudgetMs > 60000) return bad("limits.timeBudgetMs must be 1..60000");
        if (o.limits.memoryBytes < (1u << 20)) return bad("limits.memoryBytes must be at least 1048576");
        if (o.limits.patternSteps < 1000) return bad("limits.patternSteps must be at least 1000");
        if (o.stateUpdateHz < 1 || o.stateUpdateHz > 60) return bad("stateUpdateHz must be 1..60");
        if (o.storageFlushMs < 100) return bad("storageFlushMs must be at least 100");
        if (o.storageMaxBytes < 1024) return bad("storageMaxBytes must be at least 1024");
        if (o.maxTimersPerResource < 1) return bad("maxTimersPerResource must be at least 1");
        if (o.maxHandlersPerResource < 1) return bad("maxHandlersPerResource must be at least 1");
        if (o.logBurst < 1 || o.logPerSecond < 1) return bad("logBurst and logPerSecond must be at least 1");
        return true;
    }

    size_t ScriptRuntime::Refresh()
    {
        EntryScope s(*impl_);
        return impl_->Refresh();
    }

    size_t ScriptRuntime::StartAll()
    {
        EntryScope s(*impl_);
        return impl_->StartAll();
    }

    bool ScriptRuntime::Start(const std::string& name, std::string* err)
    {
        EntryScope s(*impl_);
        return impl_->Start(name, err);
    }

    bool ScriptRuntime::Stop(const std::string& name, std::string* err)
    {
        EntryScope s(*impl_);
        return impl_->Stop(name, err);
    }

    bool ScriptRuntime::Restart(const std::string& name, std::string* err)
    {
        EntryScope s(*impl_);
        return impl_->Restart(name, err);
    }

    void ScriptRuntime::Shutdown()
    {
        EntryScope s(*impl_);
        impl_->Shutdown();
    }

    std::vector<ResourceStatus> ScriptRuntime::Resources() const
    {
        return impl_->Resources();
    }

    bool ScriptRuntime::IsRunning(const std::string& name) const
    {
        return impl_->IsRunning(name);
    }

    RuntimeStats ScriptRuntime::Stats() const
    {
        return impl_->Stats();
    }

    void ScriptRuntime::Tick()
    {
        EntryScope s(*impl_);
        impl_->Tick();
    }

    ConnectVerdict ScriptRuntime::OnPlayerConnecting(const ConnectInfo& info)
    {
        EntryScope s(*impl_);
        return impl_->OnPlayerConnecting(info);
    }

    void ScriptRuntime::OnPlayerJoined(uint16_t id)
    {
        EntryScope s(*impl_);
        impl_->OnPlayerJoined(id);
    }

    void ScriptRuntime::OnPlayerLeft(const PlayerInfo& info, LeaveReason why)
    {
        EntryScope s(*impl_);
        impl_->OnPlayerLeft(info, why);
    }

    bool ScriptRuntime::OnChatMessage(uint16_t playerId, const std::string& text)
    {
        EntryScope s(*impl_);
        return impl_->OnChatMessage(playerId, text);
    }

    void ScriptRuntime::OnPlayerState(uint16_t id, const PlayerState& state)
    {
        impl_->OnPlayerState(id, state);
    }

    bool ScriptRuntime::WantsPlayerState() const
    {
        return impl_->WantsPlayerState();
    }

    ConsoleResult ScriptRuntime::ExecuteConsole(const std::string& line, std::string& output)
    {
        EntryScope s(*impl_);
        return impl_->ExecuteConsole(line, output);
    }

}
