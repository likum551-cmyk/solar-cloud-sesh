// The script-facing API: every function and table a resource can see besides the (restricted)
// standard library. docs/api/gamemode.md documents each of them for gamemode authors.
//
// Every function in this file is a lua_CFunction (or a helper of one), so it follows the Lua
// error safety rules at the top of internal.h: no object with a destructor is alive across a
// call that can raise. Temporaries that exist only for the duration of one non-raising call
// (std::string(ptr, n) passed to an IServerApi method) are fine; anything that must be alive
// while pushing results lives in a Resource scratch member.
#include "fsutil.h"
#include "internal.h"
#include "json.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace gage::scripting::detail {

    // ---- shared helpers (declared in internal.h) -------------------------------------------------

    std::string FirstPart(const std::string& text)
    {
        const size_t pos = text.find("\nstack traceback:");
        return pos == std::string::npos ? text : text.substr(0, pos);
    }

    bool IsBuiltinEvent(const std::string& event)
    {
        static const char* const kBuiltin[] = {"playerConnecting", "playerJoined",      "playerLeft",   "chatMessage",
                                               "playerStateUpdate", "resourceStart",    "resourceStop"};
        for (const char* b : kBuiltin)
            if (event == b) return true;
        return false;
    }

    std::string CleanReason(const char* p, size_t n, const std::string& fallback)
    {
        std::string s(p, n);
        if (!fsutil::Utf8Valid(s.data(), s.size())) return fallback;
        for (char& ch : s)
        {
            const unsigned char c = static_cast<unsigned char>(ch);
            if (c < 0x20 || c == 0x7F) ch = ' ';
        }
        s = fsutil::Utf8Truncate(s, 200);
        size_t a = 0, b = s.size();
        while (a < b && s[a] == ' ') a++;
        while (b > a && s[b - 1] == ' ') b--;
        s = s.substr(a, b - a);
        return s.empty() ? fallback : s;
    }

    void PushPlayerTable(lua_State* L, const PlayerInfo& p)
    {
        lua_createtable(L, 0, 7);
        lua_pushinteger(L, p.id);
        lua_setfield(L, -2, "id");
        lua_pushlstring(L, p.name.data(), p.name.size());
        lua_setfield(L, -2, "name");
        lua_pushlstring(L, p.address.data(), p.address.size());
        lua_setfield(L, -2, "ip");
        lua_pushlstring(L, p.accountId.data(), p.accountId.size());
        lua_setfield(L, -2, "accountId");
        lua_pushinteger(L, p.pingMs);
        lua_setfield(L, -2, "ping");
        lua_pushboolean(L, p.isAdmin ? 1 : 0);
        lua_setfield(L, -2, "isAdmin");
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "console");
    }

    void PushConsoleSource(lua_State* L)
    {
        lua_createtable(L, 0, 7);
        lua_pushinteger(L, -1);
        lua_setfield(L, -2, "id");
        lua_pushliteral(L, "console");
        lua_setfield(L, -2, "name");
        lua_pushliteral(L, "");
        lua_setfield(L, -2, "ip");
        lua_pushliteral(L, "console");
        lua_setfield(L, -2, "accountId");
        lua_pushinteger(L, 0);
        lua_setfield(L, -2, "ping");
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, "isAdmin");
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, "console");
    }

    void PushSource(lua_State* L, const Source& s)
    {
        if (s.console)
            PushConsoleSource(L);
        else
            PushPlayerTable(L, s.player);
    }

    namespace {

        // ---- argument helpers --------------------------------------------------------------------

        // A player argument: an integer id, or a table with an integer `id` field (what the
        // runtime hands to handlers). The console source (id -1) is refused.
        int CheckPlayerId(lua_State* L, int idx)
        {
            idx = lua_absindex(L, idx);
            lua_Integer id = -1;
            if (lua_type(L, idx) == LUA_TNUMBER && lua_isinteger(L, idx))
                id = lua_tointeger(L, idx);
            else if (lua_type(L, idx) == LUA_TTABLE)
            {
                lua_pushliteral(L, "id");
                lua_rawget(L, idx);
                if (lua_type(L, -1) == LUA_TNUMBER && lua_isinteger(L, -1)) id = lua_tointeger(L, -1);
                lua_pop(L, 1);
            }
            else
                luaL_typeerror(L, idx, "player (table or id)");
            if (id < 0 || id > 0xFFFE) luaL_argerror(L, idx, "not a valid player id");
            return static_cast<int>(id);
        }

        bool ValidEventName(const char* s, size_t n)
        {
            if (n == 0 || n > 64) return false;
            for (size_t i = 0; i < n; i++)
            {
                const unsigned char c = static_cast<unsigned char>(s[i]);
                if (!(std::isalnum(c) || c == '_' || c == ':' || c == '.' || c == '-') || c >= 0x80) return false;
            }
            return true;
        }

        bool ValidCommandName(const char* s, size_t n)
        {
            if (n == 0 || n > 32) return false;
            for (size_t i = 0; i < n; i++)
            {
                const unsigned char c = static_cast<unsigned char>(s[i]);
                if (!(std::isalnum(c) || c == '_') || c >= 0x80) return false;
            }
            return true;
        }

        bool ValidExportName(const char* s, size_t n)
        {
            if (n == 0 || n > 64) return false;
            for (size_t i = 0; i < n; i++)
            {
                const unsigned char c = static_cast<unsigned char>(s[i]);
                if (!(std::isalnum(c) || c == '_') || c >= 0x80) return false;
            }
            return true;
        }

        // Pushes `nil, <r->xferErr>` and returns 2: the standard failure result.
        int PushFailure(lua_State* L, Resource* r)
        {
            lua_pushnil(L);
            lua_pushlstring(L, r->xferErr.data(), r->xferErr.size());
            return 2;
        }

        // ---- JSON <-> Lua ------------------------------------------------------------------------

        struct LuaBuilder
        {
            struct Frame
            {
                bool obj;
                lua_Integer n;
            };
            lua_State* L;
            Frame frames[72];
            int top = -1;

            explicit LuaBuilder(lua_State* state) : L(state) {}

            // The finished value is on top of the stack; attach it to its container.
            bool Put()
            {
                if (top < 0) return true;
                Frame& f = frames[top];
                if (f.obj)
                    lua_rawset(L, -3);
                else
                    lua_rawseti(L, -2, ++f.n);
                return true;
            }
            bool Null() { PushJsonNull(L); return Put(); }
            bool Bool(bool v) { lua_pushboolean(L, v ? 1 : 0); return Put(); }
            bool Int(long long v) { lua_pushinteger(L, static_cast<lua_Integer>(v)); return Put(); }
            bool Num(double v) { lua_pushnumber(L, static_cast<lua_Number>(v)); return Put(); }
            bool Str(const char* p, size_t n) { lua_pushlstring(L, p, n); return Put(); }
            bool BeginArr()
            {
                lua_createtable(L, 0, 0);
                frames[++top] = Frame{false, 0};
                return true;
            }
            bool EndArr() { --top; return Put(); }
            bool BeginObj()
            {
                lua_createtable(L, 0, 0);
                frames[++top] = Frame{true, 0};
                return true;
            }
            // A key that already exists in the object is refused: two parsers that disagree on
            // which duplicate wins are a classic way to smuggle a value past a check.
            bool Key(const char* p, size_t n)
            {
                lua_pushlstring(L, p, n);
                lua_pushvalue(L, -1);
                lua_rawget(L, -3);
                const bool seen = !lua_isnil(L, -1);
                lua_pop(L, 1);
                if (seen)
                {
                    dup = true;
                    return false;
                }
                return true;
            }
            bool EndObj() { --top; return Put(); }
            bool dup = false;
        };

        // Decodes [text, text+n) and pushes the value. Returns true; or false with the
        // stack restored and *msg / *offset set. May raise (memory).
        bool DecodeJson(lua_State* L, Resource* r, const char* text, size_t n, char* msg, size_t msgSize, size_t* offset)
        {
            const int base = lua_gettop(L);
            luaL_checkstack(L, 2 * 66 + 10, "json.decode: stack");
            LuaBuilder b(L);
            json::Limits lim;
            json::Parser<LuaBuilder> parser(b, r->scratch, lim);
            json::ParseError e;
            if (parser.Parse(text, n, e)) return true;
            lua_settop(L, base);
            std::snprintf(msg, msgSize, "%s", b.dup ? "duplicate object key" : e.message);
            *offset = e.offset;
            return false;
        }

        // The JSON encoder (Lua value -> JSON text). Plain data only; the struct has no
        // destructor so a raise inside is safe.
        struct Enc
        {
            lua_State* L;
            std::string* out;
            bool pretty;
        };

        constexpr size_t kMaxJsonOut = 16u * 1024u * 1024u;

        void EncIndent(Enc& e, int depth)
        {
            if (e.out->size() > kMaxJsonOut) luaL_error(e.L, "json.encode: the result is larger than %d MiB", static_cast<int>(kMaxJsonOut >> 20));
            if (!e.pretty) return;
            e.out->push_back('\n');
            for (int i = 0; i < depth * 2; i++) e.out->push_back(' ');
        }

        void EncString(Enc& e, const char* s, size_t n)
        {
            if (!fsutil::Utf8Valid(s, n)) luaL_error(e.L, "json.encode: string is not valid UTF-8 (encode binary data as base64 or hex first)");
            e.out->push_back('"');
            for (size_t i = 0; i < n; i++)
            {
                const unsigned char c = static_cast<unsigned char>(s[i]);
                switch (c)
                {
                case '"': e.out->append("\\\""); break;
                case '\\': e.out->append("\\\\"); break;
                case '\n': e.out->append("\\n"); break;
                case '\r': e.out->append("\\r"); break;
                case '\t': e.out->append("\\t"); break;
                case '\b': e.out->append("\\b"); break;
                case '\f': e.out->append("\\f"); break;
                default:
                    if (c < 0x20)
                    {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        e.out->append(buf);
                    }
                    else
                        e.out->push_back(static_cast<char>(c));
                }
            }
            e.out->push_back('"');
        }

        void EncValue(Enc& e, int idx, int depth);

        struct KeyRef
        {
            const char* s;
            size_t n;
        };

        void EncTable(Enc& e, int idx, int depth)
        {
            lua_State* L = e.L;
            if (depth > 64) luaL_error(L, "json.encode: value is nested too deeply (or contains a cycle)");
            luaL_checkstack(L, 8, "json.encode: nesting");
            const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, idx));
            size_t total = 0;
            bool allInt = true, allStr = true;
            lua_pushnil(L);
            while (lua_next(L, idx) != 0)
            {
                total++;
                if (lua_type(L, -2) == LUA_TNUMBER && lua_isinteger(L, -2))
                {
                    const lua_Integer k = lua_tointeger(L, -2);
                    if (k < 1 || k > n) allInt = false;
                }
                else
                    allInt = false;
                if (lua_type(L, -2) != LUA_TSTRING) allStr = false;
                lua_pop(L, 1);
            }
            if (total == 0)
            {
                e.out->append("{}");
                return;
            }
            if (allInt && static_cast<lua_Integer>(total) == n)
            {
                e.out->push_back('[');
                for (lua_Integer i = 1; i <= n; i++)
                {
                    if (i > 1) e.out->push_back(',');
                    EncIndent(e, depth + 1);
                    lua_rawgeti(L, idx, i);
                    EncValue(e, lua_gettop(L), depth + 1);
                    lua_pop(L, 1);
                }
                EncIndent(e, depth);
                e.out->push_back(']');
                return;
            }
            if (!allStr) luaL_error(L, "json.encode: a table must be an array (keys 1..n with no holes) or an object (string keys only)");
            // Object: keys sorted bytewise so the output is deterministic. The key array lives in
            // a Lua userdata (collected even if an error unwinds this frame).
            KeyRef* keys = static_cast<KeyRef*>(lua_newuserdatauv(L, total * sizeof(KeyRef), 0));
            const int udIdx = lua_gettop(L);
            size_t i = 0;
            lua_pushnil(L);
            while (lua_next(L, idx) != 0)
            {
                size_t kn = 0;
                const char* ks = lua_tolstring(L, -2, &kn); // a string key: no conversion
                keys[i].s = ks;
                keys[i].n = kn;
                i++;
                lua_pop(L, 1);
            }
            std::sort(keys, keys + total, [](const KeyRef& a, const KeyRef& b) {
                const size_t m = a.n < b.n ? a.n : b.n;
                const int c = std::memcmp(a.s, b.s, m);
                return c != 0 ? c < 0 : a.n < b.n;
            });
            e.out->push_back('{');
            for (size_t k = 0; k < total; k++)
            {
                if (k) e.out->push_back(',');
                EncIndent(e, depth + 1);
                EncString(e, keys[k].s, keys[k].n);
                e.out->push_back(':');
                if (e.pretty) e.out->push_back(' ');
                lua_pushlstring(L, keys[k].s, keys[k].n);
                lua_rawget(L, idx);
                EncValue(e, lua_gettop(L), depth + 1);
                lua_pop(L, 1);
            }
            EncIndent(e, depth);
            e.out->push_back('}');
            lua_remove(L, udIdx);
        }

        void EncValue(Enc& e, int idx, int depth)
        {
            lua_State* L = e.L;
            idx = lua_absindex(L, idx);
            switch (lua_type(L, idx))
            {
            case LUA_TNIL: e.out->append("null"); return;
            case LUA_TBOOLEAN: e.out->append(lua_toboolean(L, idx) ? "true" : "false"); return;
            case LUA_TNUMBER:
            {
                char buf[48];
                if (lua_isinteger(L, idx))
                    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(lua_tointeger(L, idx)));
                else
                {
                    const double d = static_cast<double>(lua_tonumber(L, idx));
                    if (!std::isfinite(d)) luaL_error(L, "json.encode: cannot encode NaN or infinity");
                    json::FormatDouble(d, buf);
                }
                e.out->append(buf);
                return;
            }
            case LUA_TSTRING:
            {
                size_t n = 0;
                const char* s = lua_tolstring(L, idx, &n);
                EncString(e, s, n);
                return;
            }
            case LUA_TTABLE: EncTable(e, idx, depth); return;
            case LUA_TLIGHTUSERDATA:
                if (IsJsonNull(L, idx))
                {
                    e.out->append("null");
                    return;
                }
                break;
            default: break;
            }
            luaL_error(L, "json.encode: cannot encode a %s value", luaL_typename(L, idx));
        }

        // Encodes the value at idx into r->jsonOut (a Resource member, so an error raised
        // mid-way leaks nothing; the stack-based luaL_Buffer cannot be used because the
        // encoder pushes values between appends).
        void EncodeToMember(lua_State* L, Resource* r, int idx, bool pretty)
        {
            r->jsonOut.clear();
            Enc e{L, &r->jsonOut, pretty};
            EncValue(e, idx, 0);
        }

        // Releases the encoder's buffer when a large result left it big.
        void TrimJsonOut(Resource* r)
        {
            r->jsonOut.clear();
            if (r->jsonOut.capacity() > 262144) std::string().swap(r->jsonOut);
        }

        int L_jsonEncode(lua_State* L)
        {
            Resource* r = ResourceOf(L);
            luaL_checkany(L, 1);
            bool pretty = false;
            if (!lua_isnoneornil(L, 2))
            {
                luaL_checktype(L, 2, LUA_TTABLE);
                lua_pushliteral(L, "pretty");
                lua_rawget(L, 2);
                pretty = lua_toboolean(L, -1) != 0;
                lua_pop(L, 1);
            }
            EncodeToMember(L, r, 1, pretty);
            lua_pushlstring(L, r->jsonOut.data(), r->jsonOut.size());
            TrimJsonOut(r);
            return 1;
        }

        int L_jsonDecode(lua_State* L)
        {
            Resource* r = ResourceOf(L);
            size_t n = 0;
            const char* text = luaL_checklstring(L, 1, &n);
            char msg[128];
            size_t offset = 0;
            if (DecodeJson(L, r, text, n, msg, sizeof(msg), &offset)) return 1;
            lua_pushnil(L);
            lua_pushfstring(L, "%s (at byte %d)", msg, static_cast<int>(offset));
            return 2;
        }

        // ---- log ----------------------------------------------------------------------------------

        int LogImpl(lua_State* L, LogLevel level)
        {
            Resource* r = ResourceOf(L);
            const int n = lua_gettop(L);
            luaL_Buffer b;
            luaL_buffinit(L, &b);
            for (int i = 1; i <= n; i++)
            {
                if (i > 1) luaL_addchar(&b, '\t');
                luaL_tolstring(L, i, nullptr);
                luaL_addvalue(&b);
            }
            luaL_pushresult(&b);
            size_t len = 0;
            const char* text = lua_tolstring(L, -1, &len);
            r->rt->LogScript(*r, level, std::string(text, len));
            return 0;
        }

        int L_logInfo(lua_State* L) { return LogImpl(L, LogLevel::Info); }
        int L_logWarn(lua_State* L) { return LogImpl(L, LogLevel::Warn); }
        int L_logError(lua_State* L) { return LogImpl(L, LogLevel::Error); }

        // ---- events -------------------------------------------------------------------------------

        int L_on(lua_State* L)
        {
            Resource* r = ResourceOf(L);
            size_t n = 0;
            const char* ev = luaL_checklstring(L, 1, &n);
            luaL_checktype(L, 2, LUA_TFUNCTION);
            if (!ValidEventName(ev, n)) return luaL_argerror(L, 1, "event names are 1..64 characters of A-Z a-z 0-9 _ : . -");
            if (r->handlers.size() >= r->rt->opt.maxHandlersPerResource)
                return luaL_error(L, "too many event handlers (limit %d)", static_cast<int>(r->rt->opt.maxHandlersPerResource));
            lua_pushvalue(L, 2);
            const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
            const uint64_t id = r->nextHandlerId++;
            HandlerRec rec;
            rec.event.assign(ev, n);
            rec.ref = ref;
            r->handlerOrder[rec.event].push_back(id);
            r->handlers.emplace(id, std::move(rec));
            lua_pushinteger(L, static_cast<lua_Integer>(id));
            return 1;
        }

        int L_off(lua_State* L)
        {
            Resource* r = ResourceOf(L);
            const lua_Integer id = luaL_checkinteger(L, 1);
            auto it = r->handlers.find(static_cast<uint64_t>(id));
            if (it == r->handlers.end())
            {
                lua_pushboolean(L, 0);
                return 1;
            }
            luaL_unref(L, LUA_REGISTRYINDEX, it->second.ref);
            std::vector<uint64_t>& order = r->handlerOrder[it->second.event];
            order.erase(std::remove(order.begin(), order.end(), it->first), order.end());
            if (order.empty()) r->handlerOrder.erase(it->second.event);
            r->handlers.erase(it);
            lua_pushboolean(L, 1);
            return 1;
        }

        int L_emit(lua_State* L)
        {
            Resource* r = ResourceOf(L);
            size_t n = 0;
            const char* name = luaL_checklstring(L, 1, &n);
            if (!ValidEventName(name, n)) return luaL_argerror(L, 1, "event names are 1..64 characters of A-Z a-z 0-9 _ : . -");
            const int top = lua_gettop(L);
            int rc = 0;
            {
                const std::string event(name, n);
                if (IsBuiltinEvent(event))
                {
                    r->xferErr = "event '" + event + "' is built in and cannot be emitted by a resource";
                    rc = 1;
                }
                else
                {
                    std::vector<Value> args;
                    args.reserve(static_cast<size_t>(top > 1 ? top - 1 : 0));
                    std::string err;
                    for (int i = 2; i <= top && rc == 0; i++)
                    {
                        args.emplace_back();
                        if (!FromLua(L, i, args.back(), &err))
                        {
                            r->xferErr = "argument #" + std::to_string(i - 1) + ": " + err;
                            rc = 1;
                        }
                    }
                    if (rc == 0 && !r->rt->EmitCustom(*r, event, args, &err))
                    {
                        r->xferErr = err;
                        rc = 1;
                    }
                }
            }
            if (rc) return luaL_error(L, "emit: %s", r->xferErr.c_str());
            return 0;
        }

        int L_cancelEvent(lua_State* L)
        {
            Resource* r = ResourceOf(L);
            if (!r->cancellable) return luaL_error(L, "cancelEvent() can only be called from a handler of a cancellable event (chatMessage)");
            r->cancelled = true;
            return 0;
        }

        // deferral.reject(reason) of a playerConnecting handler.
        int L_reject(lua_State* L)
        {
            Resource* r = ResourceOf(L);
            size_t n = 0;
            const char* reason = luaL_optlstring(L, 1, "", &n);
            ConnectVerdict* v = r->rt->activeVerdict;
            if (!v) return luaL_error(L, "deferral.reject() was called after playerConnecting finished");
            if (v->accept)
            {
                v->accept = false;
                v->reason = CleanReason(reason, n, "Connection refused by the server.");
            }
            return 0;
        }

        // ---- timers -------------------------------------------------------------------------------

        int SetTimerImpl(lua_State* L, bool repeat)
        {
            Resource* r = ResourceOf(L);
            luaL_checktype(L, 1, LUA_TFUNCTION);
            const lua_Integer ms = luaL_checkinteger(L, 2);
            if (ms < (repeat ? 1 : 0) || ms > 86400000)
                return luaL_argerror(L, 2, repeat ? "interval must be 1..86400000 ms" : "delay must be 0..86400000 ms");
            if (r->timers.size() >= r->rt->opt.maxTimersPerResource)
                return luaL_error(L, "too many timers (limit %d)", static_cast<int>(r->rt->opt.maxTimersPerResource));
            lua_pushvalue(L, 1);
            const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
            const uint32_t id = r->rt->AddTimer(*r, ref, static_cast<uint32_t>(ms), repeat);
            lua_pushinteger(L, id);
            return 1;
        }

        int L_setTimeout(lua_State* L) { return SetTimerImpl(L, false); }
        int L_setInterval(lua_State* L) { return SetTimerImpl(L, true); }

        int L_clearTimer(lua_State* L)
        {
            Resource* r = ResourceOf(L);
            const lua_Integer id = luaL_checkinteger(L, 1);
            int ref = LUA_NOREF;
            const bool had = id > 0 && id <= 0xFFFFFFFFLL && r->rt->ClearTimer(*r, static_cast<uint32_t>(id), &ref);
            if (had) luaL_unref(L, LUA_REGISTRYINDEX, ref);
            lua_pushboolean(L, had ? 1 : 0);
            return 1;
        }

        // ---- commands -----------------------------------------------------------------------------

        int L_registerCommand(lua_State* L)
        {
            Resource* r = ResourceOf(L);
            size_t n = 0;
            const char* name = luaL_checklstring(L, 1, &n);
            luaL_checktype(L, 2, LUA_TFUNCTION);
            if (!ValidCommandName(name, n)) return luaL_argerror(L, 1, "command names are 1..32 characters of A-Z a-z 0-9 _");
            bool restricted = false;
            r->scratch2.clear();
            if (!lua_isnoneornil(L, 3))
            {
                luaL_checktype(L, 3, LUA_TTABLE);
                lua_pushliteral(L, "restricted");
                lua_rawget(L, 3);
                restricted = lua_toboolean(L, -1) != 0;
                lua_pop(L, 1);
                lua_pushliteral(L, "help");
                lua_rawget(L, 3);
                if (lua_type(L, -1) == LUA_TSTRING)
                {
                    size_t hn = 0;
                    const char* h = lua_tolstring(L, -1, &hn);
                    r->scratch2.assign(h, hn < 120 ? hn : 120);
                }
                lua_pop(L, 1);
            }
            r->scratch.assign(name, n);
            for (char& ch : r->scratch)
                if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
            lua_pushvalue(L, 2);
            const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
            if (!r->rt->RegisterCommand(*r, r->scratch, ref, restricted, r->scratch2, &r->xferErr))
            {
                luaL_unref(L, LUA_REGISTRYINDEX, ref);
                return luaL_error(L, "registerCommand: %s", r->xferErr.c_str());
            }
            lua_pushboolean(L, 1);
            return 1;
        }

        int L_reply(lua_State* L)
        {
            Resource* r = ResourceOf(L);
            size_t n = 0;
            const char* text = luaL_checklstring(L, 2, &n);
            luaL_checktype(L, 1, LUA_TTABLE);
            lua_pushliteral(L, "console");
            lua_rawget(L, 1);
            const bool console = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            if (console)
            {
                r->rt->ConsoleReply(std::string(text, n));
                lua_pushboolean(L, 1);
                return 1;
            }
            const int id = CheckPlayerId(L, 1);
            if (n > 200) n = fsutil::Utf8Truncate(std::string(text, n), 200).size();
            r->xferErr.clear();
            const bool ok = r->rt->api.SendChat(static_cast<uint16_t>(id), std::string(text, n), &r->xferErr);
            if (ok)
            {
                lua_pushboolean(L, 1);
                return 1;
            }
            return PushFailure(L, r);
        }

        // ---- players and server actions -------------------------------------------------------------

        int L_playersList(lua_State* L)
        {
            Resource* r = ResourceOf(L);
            r->playersScratch = r->rt->api.Players();
            const size_t n = r->playersScratch.size();
            lua_createtable(L, static_cast<int>(n), 0);
            for (size_t i = 0; i < n; i++)
            {
                PushPlayerTable(L, r->playersScratch[i]);
                lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
            }
            return 1;
        }

        int L_playersGet(lua_State* L)
        {
            Resource* r = ResourceOf(L);
            bool found = false;
            if (lua_type(L, 1) == LUA_TSTRING)
            {
                size_t n = 0;
                const char* s = lua_tolstring(L, 1, &n);
                r->scratch.assign(s, n);
                for (char& ch : r->scratch)
                    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
                r->playersScratch = r->rt->api.Players();
                for (const PlayerInfo& p : r->playersScratch)
                {
                    if (p.name.size() != r->scratch.size()) continue;
                    bool same = true;
                    for (size_t i = 0; i < p.name.size() && same; i++)
                    {
                        char c = p.name[i];
                        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                        same = c == r->scratch[i];
                    }
                    if (same)
                    {
                        r->playerScratch = p;
                        found = true;
                        break;
                    }
                }
            }
            else
            {
                const int id = CheckPlayerId(L, 1);
                found = r->rt->api.GetPlayer(static_cast<uint16_t>(id), r->playerScratch);
            }
            if (!found)
            {
                lua_pushnil(L);
                return 1;
            }
            PushPlayerTable(L, r->playerScratch);
            return 1;
        }

        int L_playersCount(lua_State* L)
        {
            Resource* r = ResourceOf(L);
            lua_pushinteger(L, static_cast<lua_Integer>(r->rt->api.Players().size()));
            return 1;
        }

        int L_sendChat(lua_State* L)
        {
            Resource* r = ResourceOf(L);
            size_t tn = 0;
            const char* text = luaL_checklstring(L, 2, &tn);
            if (tn == 0 || tn > 200) return luaL_argerror(L, 2, "text must be 1..200 bytes");
            bool toAll = false;
            int id = 0;
            if (lua_type(L, 1) == LUA_TSTRING)
            {
                size_t n = 0;
                const char* s = lua_tolstring(L, 1, &n);
                if (n == 3 && std::memcmp(s, "all", 3) == 0)
                    toAll = true;
                else
                    return luaL_argerror(L, 1, "expected a player, a player id or \"all\"");
            }
            else
                id = CheckPlayerId(L, 1);
            r->xferErr.clear();
            const bool ok = toAll ? r->rt->api.SendChatAll(std::string(text, tn), &r->xferErr)
                                  : r->rt->api.SendChat(static_cast<uint16_t>(id), std::string(text, tn), &r->xferErr);
            if (ok)
            {
                lua_pushboolean(L, 1);
                return 1;
            }
            return PushFailure(L, r);
        }

        int L_kick(lua_State* L)
        {
            Resource* r = ResourceOf(L);
            const int id = CheckPlayerId(L, 1);
            size_t n = 0;
            const char* reason = luaL_optlstring(L, 2, "", &n);
            r->xferErr.clear();
            const bool ok = r->rt->api.Kick(static_cast<uint16_t>(id), CleanReason(reason, n, "Kicked by the server."), &r->xferErr);
            if (ok)
            {
                lua_pushboolean(L, 1);
                return 1;
            }
            return PushFailure(L, r);
        }

        int L_ban(lua_State* L)
        {
            Resource* r = ResourceOf(L);
            const int id = CheckPlayerId(L, 1);
            size_t n = 0;
            const char* reason = luaL_optlstring(L, 2, "", &n);
            r->xferErr.clear();
            const bool ok = r->rt->api.Ban(static_cast<uint16_t>(id), CleanReason(reason, n, ""), &r->xferErr);
            if (ok)
            {
                lua_pushboolean(L, 1);
                return 1;
            }
            return PushFailure(L, r);
        }

        int L_banIp(lua_State* L)
        {
            Resource* r = ResourceOf(L);
            size_t ipn = 0;
            const char* ip = luaL_checklstring(L, 1, &ipn);
            size_t n = 0;
            const char* reason = luaL_optlstring(L, 2, "", &n);
            r->xferErr.clear();
            const bool ok = r->rt->api.BanIp(std::string(ip, ipn), CleanReason(reason, n, ""), &r->xferErr);
            if (ok)
            {
                lua_pushboolean(L, 1);
                return 1;
            }
            return PushFailure(L, r);
        }

        int L_unban(lua_State* L)
        {
            Resource* r = ResourceOf(L);
            size_t n = 0;
            const char* target = luaL_checklstring(L, 1, &n);
            r->xferErr.clear();
            const bool ok = r->rt->api.Unban(std::string(target, n), &r->xferErr);
            if (ok)
            {
                lua_pushboolean(L, 1);
                return 1;
            }
            return PushFailure(L, r);
        }

        int L_getPosition(lua_State* L)
        {
            Resource* r = ResourceOf(L);
            const int id = CheckPlayerId(L, 1);
            if (!r->rt->api.GetPlayer(static_cast<uint16_t>(id), r->playerScratch))
            {
                lua_pushnil(L);
                lua_pushliteral(L, "no such player");
                return 2;
            }
            if (!r->playerScratch.hasPosition)
            {
                lua_pushnil(L);
                lua_pushliteral(L, "no position yet");
                return 2;
            }
            lua_pushnumber(L, r->playerScratch.state.x);
            lua_pushnumber(L, r->playerScratch.state.y);
            lua_pushnumber(L, r->playerScratch.state.z);
            lua_pushnumber(L, r->playerScratch.state.heading);
            return 4;
        }

        int L_setPosition(lua_State* L)
        {
            Resource* r = ResourceOf(L);
            const int id = CheckPlayerId(L, 1);
            const lua_Number x = luaL_checknumber(L, 2);
            const lua_Number y = luaL_checknumber(L, 3);
            const lua_Number z = luaL_checknumber(L, 4);
            const lua_Number h = luaL_optnumber(L, 5, 0);
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(h))
                return luaL_error(L, "setPosition: coordinates must be finite numbers");
            Position pos;
            pos.x = static_cast<float>(x);
            pos.y = static_cast<float>(y);
            pos.z = static_cast<float>(z);
            pos.heading = static_cast<float>(h);
            r->xferErr.clear();
            const bool ok = r->rt->api.SetPosition(static_cast<uint16_t>(id), pos, &r->xferErr);
            if (ok)
            {
                lua_pushboolean(L, 1);
                return 1;
            }
            return PushFailure(L, r);
        }

        int L_getConvar(lua_State* L)
        {
            Resource* r = ResourceOf(L);
            size_t n = 0;
            const char* name = luaL_checklstring(L, 1, &n);
            r->scratch.clear();
            if (r->rt->api.GetConvar(std::string(name, n), r->scratch))
            {
                lua_pushlstring(L, r->scratch.data(), r->scratch.size());
                return 1;
            }
            if (lua_isnone(L, 2))
                lua_pushnil(L);
            else
                lua_pushvalue(L, 2);
            return 1;
        }

        // ---- config and storage ---------------------------------------------------------------------

        // Pushes the value stored as JSON text `v`, or raises when it is not valid (cannot happen
        // for text the runtime wrote itself).
        void PushJsonText(lua_State* L, Resource* r, const std::string& v)
        {
            char msg[128];
            size_t offset = 0;
            if (!DecodeJson(L, r, v.data(), v.size(), msg, sizeof(msg), &offset))
                luaL_error(L, "stored value is damaged: %s", msg);
        }

        int L_configGet(lua_State* L)
        {
            Resource* r = ResourceOf(L);
            size_t n = 0;
            const char* key = luaL_checklstring(L, 1, &n);
            auto it = r->manifest.config.find(std::string(key, n));
            if (it == r->manifest.config.end())
            {
                if (lua_isnone(L, 2))
                    lua_pushnil(L);
                else
                    lua_pushvalue(L, 2);
                return 1;
            }
            PushJsonText(L, r, it->second);
            return 1;
        }

        int L_configKeys(lua_State* L)
        {
            Resource* r = ResourceOf(L);
            lua_createtable(L, static_cast<int>(r->manifest.config.size()), 0);
            lua_Integer i = 1;
            for (const auto& kv : r->manifest.config)
            {
                lua_pushlstring(L, kv.first.data(), kv.first.size());
                lua_rawseti(L, -2, i++);
            }
            return 1;
        }

        int L_storageGet(lua_State* L)
        {
            Resource* r = ResourceOf(L);
            size_t n = 0;
            const char* key = luaL_checklstring(L, 1, &n);
            const std::string* v = r->storage.Get(std::string(key, n));
            if (!v)
            {
                if (lua_isnone(L, 2))
                    lua_pushnil(L);
                else
                    lua_pushvalue(L, 2);
                return 1;
            }
            PushJsonText(L, r, *v);
            return 1;
        }

        int L_storageSet(lua_State* L)
        {
            Resource* r = ResourceOf(L);
            size_t n = 0;
            const char* key = luaL_checklstring(L, 1, &n);
            luaL_checkany(L, 2);
            if (lua_isnil(L, 2))
            {
                lua_pushboolean(L, r->storage.Delete(std::string(key, n)) ? 1 : 0);
                return 1;
            }
            EncodeToMember(L, r, 2, false); // raises for values that are not plain data
            r->xferErr.clear();
            const bool ok = r->storage.Set(std::string(key, n), r->jsonOut.data(), r->jsonOut.size(), &r->xferErr);
            TrimJsonOut(r);
            if (!ok) return luaL_error(L, "storage.set: %s", r->xferErr.c_str());
            if (r->storageDirtySinceMs == 0) r->storageDirtySinceMs = r->rt->api.NowMs() | 1; // never 0 = "clean"
            lua_pushboolean(L, 1);
            return 1;
        }

        int L_storageDelete(lua_State* L)
        {
            Resource* r = ResourceOf(L);
            size_t n = 0;
            const char* key = luaL_checklstring(L, 1, &n);
            const bool had = r->storage.Delete(std::string(key, n));
            if (had && r->storageDirtySinceMs == 0) r->storageDirtySinceMs = r->rt->api.NowMs() | 1;
            lua_pushboolean(L, had ? 1 : 0);
            return 1;
        }

        int L_storageKeys(lua_State* L)
        {
            Resource* r = ResourceOf(L);
            r->stringsScratch = r->storage.Keys();
            lua_createtable(L, static_cast<int>(r->stringsScratch.size()), 0);
            for (size_t i = 0; i < r->stringsScratch.size(); i++)
            {
                lua_pushlstring(L, r->stringsScratch[i].data(), r->stringsScratch[i].size());
                lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
            }
            return 1;
        }

        int L_storageFlush(lua_State* L)
        {
            Resource* r = ResourceOf(L);
            r->xferErr.clear();
            if (r->storage.Flush(&r->xferErr))
            {
                r->storageDirtySinceMs = 0;
                lua_pushboolean(L, 1);
                return 1;
            }
            return PushFailure(L, r);
        }

        // ---- exports --------------------------------------------------------------------------------

        int L_export(lua_State* L)
        {
            Resource* r = ResourceOf(L);
            size_t n = 0;
            const char* name = luaL_checklstring(L, 1, &n);
            luaL_checktype(L, 2, LUA_TFUNCTION);
            if (!ValidExportName(name, n)) return luaL_argerror(L, 1, "export names are 1..64 characters of A-Z a-z 0-9 _");
            lua_pushvalue(L, 2);
            const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
            r->scratch.assign(name, n);
            auto it = r->exportRefs.find(r->scratch);
            if (it != r->exportRefs.end())
            {
                luaL_unref(L, LUA_REGISTRYINDEX, it->second);
                it->second = ref;
            }
            else
                r->exportRefs.emplace(r->scratch, ref);
            return 0;
        }

        // exports.<resource>.<fn>(...): the C closure behind the call; upvalues are the resource
        // name and the function name.
        int L_exportCall(lua_State* L)
        {
            Resource* r = ResourceOf(L);
            size_t tn = 0, fnn = 0;
            const char* target = lua_tolstring(L, lua_upvalueindex(1), &tn);
            const char* fn = lua_tolstring(L, lua_upvalueindex(2), &fnn);
            const int nargs = lua_gettop(L);
            int rc = 0;
            {
                std::vector<Value> args;
                args.reserve(static_cast<size_t>(nargs));
                std::string err;
                for (int i = 1; i <= nargs && rc == 0; i++)
                {
                    args.emplace_back();
                    if (!FromLua(L, i, args.back(), &err))
                    {
                        r->xferErr = "argument #" + std::to_string(i) + ": " + err;
                        rc = 1;
                    }
                }
                if (rc == 0)
                {
                    r->xfer.clear();
                    if (!r->rt->CallExport(std::string(target, tn), std::string(fn, fnn), args, r->xfer, &err))
                    {
                        r->xferErr = err;
                        rc = 1;
                    }
                }
            }
            if (rc) return luaL_error(L, "exports.%s.%s: %s", target, fn, r->xferErr.c_str());
            const int nres = static_cast<int>(r->xfer.size());
            luaL_checkstack(L, nres + 4, "too many results");
            for (int i = 0; i < nres; i++) ToLua(L, r->xfer[static_cast<size_t>(i)]);
            return nres;
        }

        // __index of one resource's proxy: exports.<resource>[fn] -> closure.
        int L_proxyIndex(lua_State* L)
        {
            luaL_checktype(L, 2, LUA_TSTRING);
            lua_pushvalue(L, lua_upvalueindex(1));
            lua_pushvalue(L, 2);
            lua_pushcclosure(L, L_exportCall, 2);
            return 1;
        }

        int L_readOnly(lua_State* L)
        {
            return luaL_error(L, "this table is read-only");
        }

        // __index of the `exports` table: exports[resource] -> proxy.
        int L_exportsIndex(lua_State* L)
        {
            luaL_checktype(L, 2, LUA_TSTRING);
            lua_newtable(L);
            lua_newtable(L);
            lua_pushvalue(L, 2);
            lua_pushcclosure(L, L_proxyIndex, 1);
            lua_setfield(L, -2, "__index");
            lua_pushcfunction(L, L_readOnly);
            lua_setfield(L, -2, "__newindex");
            lua_setmetatable(L, -2);
            return 1;
        }

        int L_print(lua_State* L) { return LogImpl(L, LogLevel::Info); }

    }

    // ---- registration ---------------------------------------------------------------------------

    void RegisterApi(lua_State* L, Resource* r)
    {
        static const luaL_Reg kGlobals[] = {
            {"on", L_on},
            {"off", L_off},
            {"emit", L_emit},
            {"cancelEvent", L_cancelEvent},
            {"setTimeout", L_setTimeout},
            {"setInterval", L_setInterval},
            {"clearTimer", L_clearTimer},
            {"registerCommand", L_registerCommand},
            {"reply", L_reply},
            {"sendChat", L_sendChat},
            {"kick", L_kick},
            {"ban", L_ban},
            {"banIp", L_banIp},
            {"unban", L_unban},
            {"getPosition", L_getPosition},
            {"setPosition", L_setPosition},
            {"getConvar", L_getConvar},
            {"export", L_export},
            {"print", L_print},
            {nullptr, nullptr},
        };
        lua_pushglobaltable(L);
        luaL_setfuncs(L, kGlobals, 0);
        lua_pop(L, 1);

        static const luaL_Reg kPlayers[] = {{"list", L_playersList}, {"get", L_playersGet}, {"count", L_playersCount}, {nullptr, nullptr}};
        luaL_newlib(L, kPlayers);
        lua_setglobal(L, "players");

        static const luaL_Reg kConfig[] = {{"get", L_configGet}, {"keys", L_configKeys}, {nullptr, nullptr}};
        luaL_newlib(L, kConfig);
        lua_setglobal(L, "config");

        static const luaL_Reg kStorage[] = {{"get", L_storageGet},       {"set", L_storageSet},     {"delete", L_storageDelete},
                                            {"keys", L_storageKeys},     {"flush", L_storageFlush}, {nullptr, nullptr}};
        luaL_newlib(L, kStorage);
        lua_setglobal(L, "storage");

        static const luaL_Reg kLog[] = {{"info", L_logInfo}, {"warn", L_logWarn}, {"error", L_logError}, {nullptr, nullptr}};
        luaL_newlib(L, kLog);
        lua_setglobal(L, "log");

        static const luaL_Reg kJson[] = {{"encode", L_jsonEncode}, {"decode", L_jsonDecode}, {nullptr, nullptr}};
        luaL_newlib(L, kJson);
        PushJsonNull(L);
        lua_setfield(L, -2, "null");
        lua_setglobal(L, "json");

        // exports: read-only, resolves exports.<resource>.<fn> lazily at call time.
        lua_newtable(L);
        lua_newtable(L);
        lua_pushcfunction(L, L_exportsIndex);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, L_readOnly);
        lua_setfield(L, -2, "__newindex");
        lua_setmetatable(L, -2);
        lua_setglobal(L, "exports");

        // resource: this resource's own manifest facts.
        lua_createtable(L, 0, 3);
        lua_pushlstring(L, r->manifest.name.data(), r->manifest.name.size());
        lua_setfield(L, -2, "name");
        lua_pushlstring(L, r->manifest.version.data(), r->manifest.version.size());
        lua_setfield(L, -2, "version");
        lua_createtable(L, static_cast<int>(r->manifest.dependencies.size()), 0);
        for (size_t i = 0; i < r->manifest.dependencies.size(); i++)
        {
            lua_pushlstring(L, r->manifest.dependencies[i].data(), r->manifest.dependencies[i].size());
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
        lua_setfield(L, -2, "dependencies");
        lua_setglobal(L, "resource");
    }

    // Used by Engine to deliver playerConnecting: builds the `deferral` argument table.
    void PushDeferral(lua_State* L)
    {
        lua_createtable(L, 0, 1);
        lua_pushcfunction(L, L_reject);
        lua_setfield(L, -2, "reject");
    }

}
