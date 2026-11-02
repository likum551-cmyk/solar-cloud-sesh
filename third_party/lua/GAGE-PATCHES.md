# Lua 5.4.9 as vendored in GAGE-MP

| | |
|---|---|
| Version | Lua 5.4.9 (released 2026-08-10), the newest 5.4 release |
| Source | https://www.lua.org/ftp/lua-5.4.9.tar.gz (373429 bytes) |
| SHA-256 | `2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6` (matches the value lua.org lists next to the file at https://www.lua.org/ftp/) |
| License | MIT, see `LICENSE` (the same text is at the end of `src/lua.h` and in `doc/readme.html`) |

What is here: `src/` (the tarball's `src/` directory, unchanged except for one patch), `README`
and `doc/readme.html` (upstream, for the license), `SHA256SUMS-upstream` (the SHA-256 of every
upstream file in `src/`), `patches/lstrlib-pattern-budget.patch`.

Verify: `cd third_party/lua && sha256sum -c SHA256SUMS-upstream` reports every file OK except
`src/lstrlib.c` (the one patched file). `patch -R -p1 < patches/lstrlib-pattern-budget.patch`
restores it, after which that line is OK too.

## What GAGE builds

Only these files are compiled (see `libs/scripting`): `lapi lcode lctype ldebug ldo ldump lfunc
lgc llex lmem lobject lopcodes lparser lstate lstring ltable ltm lundump lvm lzio lauxlib
lbaselib lcorolib lmathlib loslib lstrlib ltablib lutf8lib`. `liolib.c`, `loadlib.c`, `ldblib.c`,
`linit.c`, `lua.c` and `luac.c` are never compiled, so the `io` library, `package`/`require`
loading from disk, `package.loadlib` and the `debug` library do not exist in the binary at all
(the scripting runtime provides its own restricted `require` and a `debug.traceback`).
`loslib.c` is compiled and the runtime exposes only `os.time`, `os.clock`, `os.date` and
`os.difftime`.

## The one patch: pattern-matching budget (`GAGE_LUA_PATTERN_BUDGET`)

Why: the scripting runtime aborts runaway scripts with a `lua_sethook` instruction count, but a
count hook only fires between VM instructions. A Lua pattern such as `(".-.-.-b")` on a 100 KB
subject runs inside one C call (`string.find`) for minutes and the hook never fires, so one
script could freeze the whole server tick.

What: `lstrlib.c`'s matcher charges one unit per `match()` entry and per loop turn in
`max_expand` / `min_expand` against the global `gage_lua_pattern_steps_left`; when it reaches
0 the matcher raises the ordinary Lua error `pattern matching budget exceeded`. The runtime arms
the counter at the start of every script entry (event handler, timer, command, load) from
`ResourceLimits::patternSteps`. About 25 added lines, all behind `#if defined(GAGE_LUA_PATTERN_BUDGET)`;
without that define the file compiles to upstream behavior. Single-threaded by design (a plain
global), like the rest of the runtime.

Other C-library loops that the hook cannot interrupt are closed in `libs/scripting` instead of
by patching: `string.rep` with an empty subject and a huge count, and `table.move` with a huge
range. Everything else that can run long in C (`table.sort`, `string.rep` with data, `table.concat`)
is bounded by the per-state memory cap.
