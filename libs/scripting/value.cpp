#include "value.h"

#include "lua_include.h"

namespace gage::scripting {

    namespace {

        const char kJsonNullTag = 0;

        struct Ctx
        {
            const CopyLimits* lim;
            std::string* err;
            size_t nodes = 0;
            size_t bytes = 0;
        };

        bool Fail(Ctx& c, const std::string& why)
        {
            if (c.err && c.err->empty()) *c.err = why;
            return false;
        }

        bool Copy(lua_State* L, int idx, Value& out, Ctx& c, int depth)
        {
            idx = lua_absindex(L, idx);
            if (++c.nodes > c.lim->maxNodes) return Fail(c, "value is too large to copy");
            switch (lua_type(L, idx))
            {
            case LUA_TNIL:
                out.t = Value::T::Nil;
                return true;
            case LUA_TBOOLEAN:
                out.t = Value::T::Bool;
                out.b = lua_toboolean(L, idx) != 0;
                return true;
            case LUA_TNUMBER:
                if (lua_isinteger(L, idx))
                {
                    out.t = Value::T::Int;
                    out.i = static_cast<long long>(lua_tointeger(L, idx));
                }
                else
                {
                    out.t = Value::T::Num;
                    out.d = static_cast<double>(lua_tonumber(L, idx));
                }
                return true;
            case LUA_TSTRING:
            {
                size_t n = 0;
                const char* s = lua_tolstring(L, idx, &n); // a real string: no in-place conversion
                c.bytes += n;
                if (c.bytes > c.lim->maxBytes) return Fail(c, "value is too large to copy");
                out.t = Value::T::Str;
                out.s.assign(s, n);
                return true;
            }
            case LUA_TLIGHTUSERDATA:
                if (IsJsonNull(L, idx))
                {
                    out.t = Value::T::JsonNull;
                    return true;
                }
                return Fail(c, "cannot copy a userdata value");
            case LUA_TTABLE:
            {
                if (depth >= c.lim->maxDepth) return Fail(c, "value is nested too deeply (or contains a cycle)");
                if (!lua_checkstack(L, 4)) return Fail(c, "value is nested too deeply (Lua stack exhausted)");
                out.t = Value::T::Table;
                const int top = lua_gettop(L);
                lua_pushnil(L);
                while (lua_next(L, idx) != 0)
                {
                    const int kt = lua_type(L, -2);
                    if (kt != LUA_TNUMBER && kt != LUA_TSTRING && kt != LUA_TBOOLEAN)
                    {
                        const std::string why = std::string("unsupported table key type: ") + lua_typename(L, kt);
                        lua_settop(L, top);
                        return Fail(c, why);
                    }
                    Value k, v;
                    if (!Copy(L, -2, k, c, depth + 1) || !Copy(L, -1, v, c, depth + 1))
                    {
                        lua_settop(L, top);
                        return false;
                    }
                    out.keys.push_back(std::move(k));
                    out.vals.push_back(std::move(v));
                    lua_pop(L, 1); // the value; the key stays for lua_next
                }
                return true;
            }
            case LUA_TFUNCTION:
                return Fail(c, "cannot copy a function value");
            case LUA_TTHREAD:
                return Fail(c, "cannot copy a thread value");
            default:
                return Fail(c, "cannot copy a userdata value");
            }
        }

    }

    bool FromLua(lua_State* L, int idx, Value& out, std::string* err, const CopyLimits& limits)
    {
        if (err) err->clear();
        Ctx c;
        c.lim = &limits;
        c.err = err;
        out = Value();
        return Copy(L, idx, out, c, 0);
    }

    void ToLua(lua_State* L, const Value& v)
    {
        switch (v.t)
        {
        case Value::T::Nil: lua_pushnil(L); return;
        case Value::T::Bool: lua_pushboolean(L, v.b ? 1 : 0); return;
        case Value::T::Int: lua_pushinteger(L, static_cast<lua_Integer>(v.i)); return;
        case Value::T::Num: lua_pushnumber(L, static_cast<lua_Number>(v.d)); return;
        case Value::T::Str: lua_pushlstring(L, v.s.data(), v.s.size()); return;
        case Value::T::JsonNull: PushJsonNull(L); return;
        case Value::T::Table:
            luaL_checkstack(L, 4, "value nested too deeply");
            lua_createtable(L, 0, static_cast<int>(v.keys.size()));
            for (size_t i = 0; i < v.keys.size(); i++)
            {
                ToLua(L, v.keys[i]);
                ToLua(L, v.vals[i]);
                lua_rawset(L, -3);
            }
            return;
        }
        lua_pushnil(L);
    }

    void PushJsonNull(lua_State* L)
    {
        lua_pushlightuserdata(L, const_cast<char*>(&kJsonNullTag));
    }

    bool IsJsonNull(lua_State* L, int idx)
    {
        return lua_islightuserdata(L, idx) && lua_touserdata(L, idx) == &kJsonNullTag;
    }

}
