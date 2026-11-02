// Value: a Lua value copied out of one lua_State into plain C++ memory so it can be copied
// into another one (exports, custom events) without ever sharing a table between resources.
//
// Only plain data crosses: nil, boolean, number (integer or float), string, tables of those
// (raw contents, metatables are ignored) and the json.null sentinel. Functions, userdata,
// threads and tables nested deeper than kMaxDepth (a cycle shows up as "too deep") are errors.
//
// Thread-safety: none needed, single server thread. Lua error safety: FromLua never raises a
// Lua error (it uses only raw, non-allocating API calls and checks stack space), so it may be
// called from any C++ frame. ToLua can raise a Lua memory error and MUST run inside a
// protected call of the destination state (the runtime's Run() trampoline does).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct lua_State;

namespace gage::scripting {

    struct Value
    {
        enum class T : uint8_t { Nil, Bool, Int, Num, Str, Table, JsonNull };
        T t = T::Nil;
        bool b = false;
        long long i = 0;
        double d = 0;
        std::string s;
        std::vector<Value> keys; // Table: parallel arrays of keys and values
        std::vector<Value> vals;
    };

    // Limits of one copy: nesting depth, total values, total string bytes.
    struct CopyLimits
    {
        int maxDepth = 32;
        size_t maxNodes = 100000;
        size_t maxBytes = 4u * 1024u * 1024u;
    };

    // Copies the value at stack index `idx` of `L` into `out`. Returns true on success; false
    // with a reason ("cannot copy a function value", "value is nested too deeply (or contains
    // a cycle)", "value is too large to copy", "unsupported table key type: function") in
    // *err. The Lua stack is unchanged. Never raises a Lua error.
    bool FromLua(lua_State* L, int idx, Value& out, std::string* err, const CopyLimits& limits = CopyLimits());

    // Pushes `v` onto the stack of `L` as a fresh Lua value (tables are new tables, strings
    // are new strings). May raise a Lua memory error: call only inside a protected call.
    void ToLua(lua_State* L, const Value& v);

    // The json.null sentinel: a light userdata with a fixed address, equal to itself, pushed by
    // json.decode for JSON null and accepted by json.encode and by FromLua.
    void PushJsonNull(lua_State* L);
    // True when the value at `idx` is the json.null sentinel.
    bool IsJsonNull(lua_State* L, int idx);

}
