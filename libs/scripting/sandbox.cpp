// The sandbox: creating a resource's lua_State (custom allocator with a memory cap, instruction
// and time budget through lua_sethook, a whitelist of libraries), the protected-call helpers
// Run() and Invoke(), and the restricted `require`.
//
// Everything here follows the Lua error safety rules at the top of internal.h.
#include "fsutil.h"
#include "internal.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gage::scripting::detail {

    namespace {

        const char kModulesKey[] = "gage.modules"; // registry: loaded modules by name
        const char kLoadingTag = 0;                // light userdata marking "being loaded"

        int64_t NowNs()
        {
            using namespace std::chrono;
            return static_cast<int64_t>(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
        }

        // ---- allocator: refuses to grow past the cap -------------------------------------------

        void* AllocFn(void* ud, void* ptr, size_t osize, size_t nsize)
        {
            MemState* m = static_cast<MemState*>(ud);
            if (ptr == nullptr) osize = 0; // with a null block Lua passes an object tag, not a size
            if (nsize == 0)
            {
                if (ptr)
                {
                    m->used -= osize;
                    std::free(ptr);
                }
                return nullptr;
            }
            if (nsize > osize)
            {
                const size_t grow = nsize - osize;
                if (grow > m->limit || m->used > m->limit - grow)
                {
                    m->exceeded = true;
                    return nullptr; // Lua raises "not enough memory"
                }
            }
            void* np = std::realloc(ptr, nsize);
            if (np == nullptr) return nullptr;
            m->used = m->used - osize + nsize;
            if (m->used > m->peak) m->peak = m->used;
            return np;
        }

        // An unprotected Lua error means a bug in the glue (rule 1 of internal.h): there is no
        // way to continue safely.
        int PanicFn(lua_State* L)
        {
            const char* msg = lua_tostring(L, -1);
            std::fprintf(stderr, "FATAL: unprotected Lua error in the scripting runtime: %s\n", msg ? msg : "(no message)");
            std::fflush(stderr);
            std::abort();
        }

        // warn() from scripts and Lua's own warnings (errors in finalizers, ...) become log lines.
        void WarnFn(void* ud, const char* msg, int tocont)
        {
            Resource* r = static_cast<Resource*>(ud);
            if (r->warnBuf.empty() && msg[0] == '@' && !tocont) return; // "@on" / "@off" control message
            r->warnBuf += msg;
            if (tocont) return;
            std::string text;
            text.swap(r->warnBuf);
            r->rt->LogScript(*r, LogLevel::Warn, text);
        }

        // ---- the budget hook -------------------------------------------------------------------

        // Runs every kHookCount VM instructions of every thread of the state. Once the budget
        // is exceeded it raises on EVERY instruction (count 1) until the entry returns, so a
        // script that catches the error with pcall is aborted again at its next instruction.
        void HookFn(lua_State* L, lua_Debug*)
        {
            Resource* r = ResourceOf(L);
            Budget& b = r->budget;
            if (!b.armed) return;
            if (!b.tripped)
            {
                const int cnt = lua_gethookcount(L);
                b.used += static_cast<uint64_t>(cnt > 0 ? cnt : 1);
                if (cnt != kHookCount) lua_sethook(L, HookFn, LUA_MASKCOUNT, kHookCount); // a coroutine left at count 1 by an earlier trip
                if (b.used > b.limit)
                {
                    std::snprintf(b.reason, sizeof(b.reason), "instruction budget exceeded (%llu instructions)",
                                  static_cast<unsigned long long>(b.limit));
                    b.tripped = true;
                }
                else if (NowNs() > b.deadlineNs)
                {
                    std::snprintf(b.reason, sizeof(b.reason), "time budget exceeded (%u ms)", b.timeMs);
                    b.tripped = true;
                }
            }
            if (b.tripped)
            {
                lua_sethook(L, HookFn, LUA_MASKCOUNT, 1);
                // The abort may have been raised inside a coroutine; the main thread, which a
                // script catching it with coroutine.resume() continues on, must trip at once too.
                if (r->L != L) lua_sethook(r->L, HookFn, LUA_MASKCOUNT, 1);
                luaL_error(L, "%s", b.reason);
            }
        }

        // ---- protected-call plumbing -----------------------------------------------------------

        // The message handler of every protected call: turns the error object into a string with
        // a traceback (like the stand-alone lua interpreter does).
        int MsgHandler(lua_State* L)
        {
            const char* msg = lua_tostring(L, 1);
            if (msg == nullptr)
            {
                if (luaL_callmeta(L, 1, "__tostring") && lua_type(L, -1) == LUA_TSTRING) return 1;
                msg = lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
            }
            luaL_traceback(L, L, msg, 1);
            return 1;
        }

        struct RunCtx
        {
            void (*fn)(lua_State*, void*);
            void* arg;
        };

        int Trampoline(lua_State* L)
        {
            RunCtx* c = static_cast<RunCtx*>(lua_touserdata(L, 1));
            c->fn(L, c->arg);
            return 0;
        }

        // ---- library wrappers ------------------------------------------------------------------

        // Calls the original function stored in upvalue 1 with the `n` arguments the wrapper
        // received (still at stack indices 1..n) and returns all its results.
        int CallOriginal(lua_State* L, int n)
        {
            lua_pushvalue(L, lua_upvalueindex(1));
            for (int i = 1; i <= n; i++) lua_pushvalue(L, i);
            lua_call(L, n, LUA_MULTRET);
            return lua_gettop(L) - n;
        }

        // load(): text chunks only. Binary chunks can crash the VM and are the classic sandbox
        // escape, so the mode is forced to "t".
        int SafeLoad(lua_State* L)
        {
            luaL_checkany(L, 1);
            const int n = lua_gettop(L);
            lua_pushvalue(L, lua_upvalueindex(1));
            lua_pushvalue(L, 1);
            if (n >= 2)
                lua_pushvalue(L, 2);
            else
                lua_pushnil(L);
            lua_pushliteral(L, "t");
            int nargs = 3;
            if (n >= 4)
            {
                lua_pushvalue(L, 4);
                nargs = 4;
            }
            lua_call(L, nargs, LUA_MULTRET);
            return lua_gettop(L) - n;
        }

        // collectgarbage(): only the read-only and one-shot options. "stop", "generational", ...
        // would let a script change how the collector spends the server's time.
        int SafeCollect(lua_State* L)
        {
            const char* opt = luaL_optstring(L, 1, "collect");
            if (std::strcmp(opt, "collect") != 0 && std::strcmp(opt, "count") != 0 && std::strcmp(opt, "step") != 0 &&
                std::strcmp(opt, "isrunning") != 0)
                return luaL_error(L, "collectgarbage option '%s' is not available in resources", opt);
            return CallOriginal(L, lua_gettop(L));
        }

        // setmetatable(): refuses a metatable with __gc. Finalizers run with debug hooks turned
        // off (lgc.c GCTM), so a `while true do end` in __gc could not be interrupted by the
        // budget and would freeze the server inside a garbage-collection step.
        int SafeSetMetatable(lua_State* L)
        {
            const int n = lua_gettop(L);
            if (n >= 2 && lua_type(L, 2) == LUA_TTABLE)
            {
                lua_pushliteral(L, "__gc");
                lua_rawget(L, 2);
                const bool hasGc = !lua_isnil(L, -1);
                lua_pop(L, 1);
                if (hasGc) return luaL_error(L, "__gc metamethods are not allowed in resources");
            }
            return CallOriginal(L, n);
        }

        // string.rep(): with an empty subject and separator the C loop runs `n` times and the
        // count hook cannot interrupt it (string.rep("", 1e18) would hang the tick). Everything
        // else is bounded by the memory cap, which fails the up-front allocation.
        int SafeRep(lua_State* L)
        {
            size_t l = 0, lsep = 0;
            luaL_checklstring(L, 1, &l);
            luaL_checkinteger(L, 2);
            luaL_optlstring(L, 3, "", &lsep);
            if (l == 0 && lsep == 0)
            {
                lua_pushliteral(L, "");
                return 1;
            }
            return CallOriginal(L, lua_gettop(L));
        }

        // table.move(): the element loop is C and unbounded in the range, so cap the range.
        int SafeMove(lua_State* L)
        {
            const lua_Integer f = luaL_checkinteger(L, 2);
            const lua_Integer e = luaL_checkinteger(L, 3);
            if (e >= f)
            {
                const bool overflow = f <= 0 && e >= LUA_MAXINTEGER + f;
                if (overflow || e - f >= kMaxMoveCount)
                    return luaL_error(L, "table.move: range is too large (limit %d elements)", static_cast<int>(kMaxMoveCount));
            }
            return CallOriginal(L, lua_gettop(L));
        }

        // debug.traceback([thread,] [message [, level]]): the only debug function resources get.
        int DebugTraceback(lua_State* L)
        {
            int arg = 0;
            lua_State* L1 = L;
            if (lua_isthread(L, 1))
            {
                arg = 1;
                L1 = lua_tothread(L, 1);
            }
            const char* msg = lua_tostring(L, arg + 1);
            if (msg == nullptr && !lua_isnoneornil(L, arg + 1))
                lua_pushvalue(L, arg + 1); // a non-string message is returned untouched
            else
            {
                const int level = static_cast<int>(luaL_optinteger(L, arg + 2, (L == L1) ? 1 : 0));
                luaL_traceback(L, L1, msg, level);
            }
            return 1;
        }

        // ---- require ---------------------------------------------------------------------------

        bool ValidModuleName(const char* s, size_t n)
        {
            if (n == 0 || n > 128) return false;
            bool segStart = true;
            for (size_t i = 0; i < n; i++)
            {
                const unsigned char c = static_cast<unsigned char>(s[i]);
                if (c == '.')
                {
                    if (segStart) return false; // leading dot or ".."
                    segStart = true;
                    continue;
                }
                if (!(std::isalnum(c) || c == '_' || c == '-') || c >= 0x80) return false;
                segStart = false;
            }
            return !segStart;
        }

        // Finds and reads module `mod` ("a.b" -> a/b.lua or a/b/init.lua) inside the resource
        // folder. On success r->scratch is the chunk name ("@res/a/b.lua") and r->scratch2 the
        // source; on failure r->xferErr says why. Never raises (no Lua calls).
        bool ReadModuleSource(Resource* r, const std::string& mod)
        {
            std::string rel1 = mod;
            std::replace(rel1.begin(), rel1.end(), '.', '/');
            const std::string rel2 = rel1 + "/init.lua";
            rel1 += ".lua";
            std::string path, why1, why2;
            const std::string* chosen = nullptr;
            if (fsutil::ResolveInside(r->dir, rel1, &path, &why1))
                chosen = &rel1;
            else if (fsutil::ResolveInside(r->dir, rel2, &path, &why2))
                chosen = &rel2;
            if (!chosen)
            {
                r->xferErr = "module '" + mod + "' not found in resource '" + r->name + "' (looked for " + rel1 + " and " + rel2 + ")";
                for (const std::string* why : {&why1, &why2})
                    if (why->find("does not exist") == std::string::npos && !why->empty())
                        r->xferErr += ": " + *why;
                return false;
            }
            std::string content, why;
            if (!fsutil::ReadFile(path, content, &why, nullptr, kMaxScriptBytes))
            {
                r->xferErr = "cannot read module '" + mod + "': " + why;
                return false;
            }
            r->scratch = "@" + r->name + "/" + *chosen;
            r->scratch2 = fsutil::StripBom(content);
            return true;
        }

        // require(name): loads a module from the resource's own folder only. Names are dotted
        // identifiers, so "../x", "/etc/passwd", "a/b" and "a\\b" are all refused before any
        // path is built.
        int L_require(lua_State* L)
        {
            Resource* r = ResourceOf(L);
            size_t n = 0;
            const char* mod = luaL_checklstring(L, 1, &n);
            if (!ValidModuleName(mod, n))
                return luaL_error(L, "invalid module name (use dotted names such as 'lib.util'; '..', '/' and '\\' are not allowed)");
            lua_getfield(L, LUA_REGISTRYINDEX, kModulesKey);
            const int modules = lua_gettop(L);
            lua_getfield(L, modules, mod);
            if (!lua_isnil(L, -1))
            {
                if (lua_islightuserdata(L, -1) && lua_touserdata(L, -1) == &kLoadingTag)
                    return luaL_error(L, "loop or previous error loading module '%s'", mod);
                return 1;
            }
            lua_pop(L, 1);

            if (!ReadModuleSource(r, std::string(mod, n))) return luaL_error(L, "%s", r->xferErr.c_str());

            lua_pushlightuserdata(L, const_cast<char*>(&kLoadingTag));
            lua_setfield(L, modules, mod);
            const int st = luaL_loadbufferx(L, r->scratch2.data(), r->scratch2.size(), r->scratch.c_str(), "t");
            r->scratch2.clear();
            if (st != LUA_OK)
            {
                lua_pushnil(L);
                lua_setfield(L, modules, mod);
                return lua_error(L); // the compile error is on top
            }
            lua_pushcfunction(L, MsgHandler);
            lua_insert(L, -2);                  // msgh, chunk
            const int msgh = lua_gettop(L) - 1;
            lua_pushvalue(L, 1);                // the chunk receives the module name
            const int rc = lua_pcall(L, 1, 1, msgh);
            if (rc != LUA_OK)
            {
                lua_pushnil(L);
                lua_setfield(L, modules, mod);
                return lua_error(L); // re-raise: the budget error must keep propagating
            }
            if (!lua_isnil(L, -1))
            {
                lua_pushvalue(L, -1);
                lua_setfield(L, modules, mod);
            }
            else
            {
                lua_getfield(L, modules, mod);
                const bool stillLoading = lua_islightuserdata(L, -1) && lua_touserdata(L, -1) == &kLoadingTag;
                lua_pop(L, 1);
                if (stillLoading)
                {
                    lua_pushboolean(L, 1);
                    lua_setfield(L, modules, mod);
                }
                lua_getfield(L, modules, mod);
                return 1;
            }
            return 1;
        }

        // ---- state setup -----------------------------------------------------------------------

        void OpenLib(lua_State* L, const char* name, lua_CFunction f)
        {
            luaL_requiref(L, name, f, 1);
            lua_pop(L, 1);
        }

        // Replaces global `name` by a C closure wrapping the original function.
        void WrapGlobal(lua_State* L, const char* name, lua_CFunction wrapper)
        {
            lua_getglobal(L, name);
            lua_pushcclosure(L, wrapper, 1);
            lua_setglobal(L, name);
        }

        // Replaces field `name` of the table on top of the stack by a wrapper closure.
        void WrapField(lua_State* L, const char* name, lua_CFunction wrapper)
        {
            lua_getfield(L, -1, name);
            lua_pushcclosure(L, wrapper, 1);
            lua_setfield(L, -2, name);
        }

        void SetupBody(lua_State* L, void* ud)
        {
            Resource* r = static_cast<Resource*>(ud);
            OpenLib(L, LUA_GNAME, luaopen_base);
            OpenLib(L, LUA_COLIBNAME, luaopen_coroutine);
            OpenLib(L, LUA_TABLIBNAME, luaopen_table);
            OpenLib(L, LUA_STRLIBNAME, luaopen_string);
            OpenLib(L, LUA_MATHLIBNAME, luaopen_math);
            OpenLib(L, LUA_UTF8LIBNAME, luaopen_utf8);
            OpenLib(L, LUA_OSLIBNAME, luaopen_os);

            // base: no file access, text chunks only, no GC control beyond collect/count/step.
            lua_pushnil(L);
            lua_setglobal(L, "dofile");
            lua_pushnil(L);
            lua_setglobal(L, "loadfile");
            WrapGlobal(L, "load", SafeLoad);
            WrapGlobal(L, "collectgarbage", SafeCollect);
            WrapGlobal(L, "setmetatable", SafeSetMetatable);

            // os: time and date only (no execute, remove, rename, tmpname, getenv, exit, setlocale).
            lua_getglobal(L, LUA_OSLIBNAME);
            lua_createtable(L, 0, 4);
            static const char* const kOsAllowed[] = {"time", "clock", "date", "difftime"};
            for (const char* fname : kOsAllowed)
            {
                lua_getfield(L, -2, fname);
                lua_setfield(L, -2, fname);
            }
            lua_remove(L, -2);
            lua_pushvalue(L, -1);
            lua_setglobal(L, LUA_OSLIBNAME);
            lua_pop(L, 1);

            // string: no dump (bytecode), bounded rep.
            lua_getglobal(L, LUA_STRLIBNAME);
            lua_pushnil(L);
            lua_setfield(L, -2, "dump");
            WrapField(L, "rep", SafeRep);
            lua_pop(L, 1);

            // table: bounded move.
            lua_getglobal(L, LUA_TABLIBNAME);
            WrapField(L, "move", SafeMove);
            lua_pop(L, 1);

            // debug: traceback only. io and package are never opened (and not compiled in).
            lua_createtable(L, 0, 1);
            lua_pushcfunction(L, DebugTraceback);
            lua_setfield(L, -2, "traceback");
            lua_setglobal(L, "debug");

            // require, restricted to the resource folder.
            lua_newtable(L);
            lua_setfield(L, LUA_REGISTRYINDEX, kModulesKey);
            lua_pushcfunction(L, L_require);
            lua_setglobal(L, "require");

            RegisterApi(L, r);
        }

    }

    // ---- Resource ------------------------------------------------------------------------------

    bool Resource::Run(void (*fn)(lua_State*, void*), void* arg, std::string* err)
    {
        if (!L)
        {
            if (err) *err = "resource has no Lua state";
            return false;
        }
        const int base = lua_gettop(L);
        if (!lua_checkstack(L, 4))
        {
            if (err) *err = "Lua stack exhausted";
            return false;
        }
        lua_pushcfunction(L, MsgHandler);
        lua_pushcfunction(L, Trampoline);
        RunCtx ctx{fn, arg};
        lua_pushlightuserdata(L, &ctx);
        const int st = lua_pcall(L, 1, 0, base + 1);
        if (st != LUA_OK)
        {
            if (err)
            {
                const char* m = lua_tostring(L, -1);
                *err = m ? m : "(unknown error)";
            }
        }
        lua_settop(L, base);
        return st == LUA_OK;
    }

    bool Resource::Invoke(int nargs, int nresults, const char* what, std::string* errOut)
    {
        const int fnidx = lua_gettop(L) - nargs;
        if (!lua_checkstack(L, 2))
        {
            lua_settop(L, fnidx - 1);
            if (errOut) *errOut = "Lua stack exhausted";
            return false;
        }
        lua_pushcfunction(L, MsgHandler);
        lua_insert(L, fnidx);
        const bool outer = (budget.depth++ == 0);
        if (outer) rt->ArmBudget(*this);
        const int st = lua_pcall(L, nargs, nresults, fnidx);
        budget.depth--;
        bool tripped = false;
        std::string trippedReason;
        if (outer)
        {
            tripped = budget.tripped;
            trippedReason = budget.reason;
            rt->DisarmBudget(*this);
        }
        lua_remove(L, fnidx); // the message handler (results, if any, stay above it)
        std::string text;
        if (st != LUA_OK)
        {
            const char* m = lua_tostring(L, -1);
            text = m ? m : "(unknown error)";
            lua_pop(L, 1);
        }
        if (outer) rt->NoteEntry(*this, tripped, what, tripped ? trippedReason + "\n" + text : std::string());
        if (st != LUA_OK && !tripped)
            rt->LogScript(*this, LogLevel::Error, std::string("error in ") + what + ": " + text);
        if (errOut) *errOut = (st == LUA_OK) ? std::string() : FirstPart(text);
        return st == LUA_OK;
    }

    // ---- Engine: state and budget --------------------------------------------------------------

    bool Engine::CreateState(Resource& r, std::string* err)
    {
        r.mem = MemState();
        r.mem.limit = opt.limits.memoryBytes;
        lua_State* L = lua_newstate(AllocFn, &r.mem);
        if (!L)
        {
            if (err) *err = "cannot create the Lua state (memory limit too small?)";
            return false;
        }
        *reinterpret_cast<Resource**>(lua_getextraspace(L)) = &r;
        lua_atpanic(L, PanicFn);
        lua_setwarnf(L, WarnFn, &r);
        lua_sethook(L, HookFn, LUA_MASKCOUNT, kHookCount);
        r.L = L;
        std::string why;
        if (!r.Run(&SetupBody, &r, &why))
        {
            if (err) *err = "cannot set up the Lua state: " + FirstPart(why);
            lua_close(L);
            r.L = nullptr;
            return false;
        }
        return true;
    }

    void Engine::CloseState(Resource& r)
    {
        if (!r.L) return;
        lua_close(r.L);
        r.L = nullptr;
        if (r.mem.used != 0)
            LogSys(LogLevel::Error, "memory accounting mismatch: " + std::to_string(r.mem.used) + " bytes still counted after closing '" + r.name + "'");
    }

    void Engine::ArmBudget(Resource& r)
    {
        Budget& b = r.budget;
        b.armed = true;
        b.tripped = false;
        b.used = 0;
        b.limit = opt.limits.instructionBudget;
        b.timeMs = opt.limits.timeBudgetMs;
        b.deadlineNs = NowNs() + static_cast<int64_t>(opt.limits.timeBudgetMs) * 1000000;
        b.reason[0] = 0;
        b.savedPatternSteps = gage_lua_pattern_steps_left;
        gage_lua_pattern_steps_left = static_cast<unsigned long>(std::min<uint64_t>(opt.limits.patternSteps, ULONG_MAX));
        r.mem.exceeded = false;
        lua_sethook(r.L, HookFn, LUA_MASKCOUNT, kHookCount); // main thread back to normal granularity
    }

    void Engine::DisarmBudget(Resource& r)
    {
        r.budget.armed = false;
        gage_lua_pattern_steps_left = r.budget.savedPatternSteps;
    }

    void Engine::NoteEntry(Resource& r, bool tripped, const char* what, const std::string& trippedText)
    {
        if (r.mem.exceeded)
        {
            LogSys(LogLevel::Warn, "resource '" + r.name + "' hit its memory limit of " + std::to_string(opt.limits.memoryBytes) +
                                       " bytes in " + what + " (an allocation was refused)");
            r.mem.exceeded = false;
        }
        if (!tripped)
        {
            r.violations = 0;
            return;
        }
        r.violations++;
        LogSys(LogLevel::Error, "resource '" + r.name + "' aborted in " + what + ": " + trippedText + "\n(violation " +
                                    std::to_string(r.violations) + " in a row)");
        const uint32_t maxV = opt.limits.maxConsecutiveViolations;
        if (maxV > 0 && r.violations >= maxV && !r.faulted)
        {
            r.faulted = true;
            pendingStops.push_back(r.name);
            LogSys(LogLevel::Error, "resource '" + r.name + "' exceeded its budget " + std::to_string(r.violations) +
                                        " times in a row and will be stopped");
        }
    }

}
