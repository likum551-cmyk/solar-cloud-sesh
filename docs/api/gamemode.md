# GAGE gamemode scripting reference

This is the reference for writing a gamemode for GAGE-MP: server-side scripts in Lua 5.4 that react to
players joining, chatting and leaving, register commands, run timers, keep persistent data and talk to
each other. The code is the `gage_scripting` library in `libs/scripting/`; the embedded Lua is vendored in
`third_party/lua/` (Lua 5.4.9, MIT). It describes what the shipped code does; limits are the limits the
code enforces.

**Status: the runtime is implemented and tested (`tests/scripting_test.cpp`), but `gage-server` does not
call it yet.** Wiring it into the server is a handful of calls, listed in
[section 13](#13-embedding-the-runtime-for-server-developers). Until that lands, nothing in this document
runs inside a real server; everything in it runs in the test against a fake server.

Contents

1. [Concepts](#1-concepts)
2. [Resources](#2-resources)
3. [The Lua environment](#3-the-lua-environment)
4. [Events](#4-events)
5. [Players and the server](#5-players-and-the-server)
6. [Commands](#6-commands)
7. [Timers](#7-timers)
8. [Storage, config and convars](#8-storage-config-and-convars)
9. [Logging and JSON](#9-logging-and-json)
10. [Talking between resources](#10-talking-between-resources)
11. [Limits and what happens when a script misbehaves](#11-limits-and-what-happens-when-a-script-misbehaves)
12. [A complete example](#12-a-complete-example)
13. [Embedding the runtime (for server developers)](#13-embedding-the-runtime-for-server-developers)
14. [What is not there yet](#14-what-is-not-there-yet)

---

## 1. Concepts

* A **resource** is a folder with a `resource.json` and Lua files. A server runs many resources; each
  one is a gamemode, a library or a feature.
* **Every resource has its own Lua state.** Globals, `string` and `os` modifications and every table are
  private to it. Resources cooperate only through [exports and custom events](#10-talking-between-resources),
  which copy their values.
* **Everything runs on the server's tick thread**, one entry at a time: an event handler, a timer callback,
  a command handler, an export call, or the loading of your script files. There are no threads, no
  `wait()` and no blocking calls. Do work in short pieces and use [timers](#7-timers) to continue later.
  Each entry has a [budget](#11-limits-and-what-happens-when-a-script-misbehaves): a loop that runs too
  long is aborted and logged, it cannot freeze the server.
* **Errors are contained.** An error in a handler is logged with the resource name and a traceback. The
  other handlers, the other resources and the server carry on.
* Player objects are **snapshots**, plain tables. They do not update; call `players.get(id)` for fresh
  values.

---

## 2. Resources

### Layout

```
resources/
  welcome/
    resource.json     required
    config.json       optional, overrides "config" from resource.json
    main.lua          listed in "server_scripts"
    lib/util.lua      loaded with require("lib.util")
data/
  welcome/storage.json    written by the runtime (see section 8)
```

The folder names are set by the server: `resources/` and `data/` next to where the server runs (both are
`RuntimeOptions` fields).

### resource.json

One JSON object. Unknown keys and wrong types are errors, so a typo never silently changes behaviour.

| Key | Type | Required | Rules |
|---|---|---|---|
| `name` | string | yes | 1..64 of `A-Z a-z 0-9 _ -`, not a Windows device name (`con`, `nul`, `com1`...). **Must equal the folder name.** |
| `version` | string | yes | 1..32 printable ASCII characters, informational (`resource.version`). |
| `description` | string | no | up to 200 bytes of UTF-8. |
| `author` | string | no | up to 64 bytes of UTF-8. |
| `server_scripts` | array of string | no | Relative `.lua` paths, run in this order. No `..`, no absolute paths, no `\`, no drive letters. At most 64. |
| `client_files` | array of string | no | Reserved for the client engine; validated and not used yet. |
| `dependencies` | array of string | no | Resources that must be running first. At most 32. A resource cannot depend on itself. |
| `config` | object | no | Defaults for [`config.get`](#8-storage-config-and-convars). Any JSON. |

An error reads like `resource.json: unknown key "server_script" (known: name, version, ...)` and appears in
the log, and as `Failed` in the resource list.

`config.json` (optional, one JSON object) is laid over `config` **key by key at the top level**: a key in
`config.json` replaces the same key from `resource.json` completely; other keys are kept. Use it for
per-server settings so `resource.json` can stay in version control.

### Load order and dependencies

On server start every valid resource is started, **dependencies first, otherwise in name order**. A
resource whose dependency is missing, invalid, failed to start, or is part of a cycle is **not** started and
is reported `Blocked` with the reason (`dependency cycle: a -> b -> a`); unrelated resources start normally.
Starting a resource by hand starts its dependencies first.

### Lifecycle and console commands

| Console command | Effect |
|---|---|
| `start <resource>` | Re-reads its manifest and starts it (and its dependencies). |
| `stop <resource>` | Stops it **and every running resource that depends on it** (dependents first). |
| `restart <resource>` | Stop, start, and start again the dependents that the stop took down. The manifest and scripts are re-read, so edits take effect. |
| `refresh` | Re-scans `resources/` for new, removed or changed manifests. Running resources are not touched. |
| `resources` | Lists every known resource with its state: `started`, `stopped`, `failed` or `blocked`, and why. |

A command any resource registered with `registerCommand` can be typed there too, as the console (which
counts as an administrator).

When a resource stops, it first receives the `resourceStop` event, its storage is written, and then its
whole Lua state is closed: **every handler, timer, command and export it registered disappears with it**,
and a restart does not duplicate anything. A resource whose start fails (syntax error, error at load,
damaged storage, loop at load) leaves nothing registered.

---

## 3. The Lua environment

You get Lua 5.4 with these libraries: `base`, `coroutine`, `table`, `string`, `math`, `utf8`, plus a
restricted `os` and `debug`. Specifically:

| Available | Changed or removed |
|---|---|
| `print` (goes to the log), `pairs`, `ipairs`, `pcall`, `xpcall`, `error`, `assert`, `select`, `type`, `tostring`, `tonumber`, `next`, `rawget`/`rawset`/`rawequal`/`rawlen`, `getmetatable` | `dofile` and `loadfile` do not exist. |
| `load` | Text chunks only: a binary chunk is refused even if you ask for mode `"b"`. |
| `setmetatable` | Refuses a metatable that has `__gc` (finalizers cannot be interrupted by the [budget](#11-limits-and-what-happens-when-a-script-misbehaves)). |
| `collectgarbage("collect" \| "count" \| "step" \| "isrunning")` | Every other option is an error. |
| `coroutine.*`, `table.*`, `math.*`, `utf8.*` | `table.move` refuses a range over 4,194,304 elements. |
| `string.*` | `string.dump` is removed. `string.rep` with an empty subject returns `""` at once. Pattern matching (`find`, `match`, `gmatch`, `gsub`) has a [step budget](#11-limits-and-what-happens-when-a-script-misbehaves). |
| `os.time`, `os.clock`, `os.date`, `os.difftime` | No `os.execute`, `os.remove`, `os.rename`, `os.tmpname`, `os.getenv`, `os.exit`, `os.setlocale`. |
| `debug.traceback` | No other `debug` function. |
| `require` | See below. |
| | **No `io`, no `package`** (not even `package.loadlib`). |

### require

`require("util")` loads `util.lua` from your resource folder, `require("lib.util")` loads `lib/util.lua`, and
`require("lib")` loads `lib/init.lua` when there is no `lib.lua`. The result is cached per resource. Module
names may only contain letters, digits, `_`, `-` and dots between segments: `../x`, `/x`, `x\y`, `a..b` and
drive letters are refused. A symbolic link or Windows junction anywhere below the resource folder is
refused. A circular `require` is an error. A module can use every global of the resource (`log`, `storage`,
...). Source files are limited to 1 MiB.

### What can be passed between resources

Exports, custom events and `json` move **plain data**: `nil`, booleans, numbers (integers stay integers),
strings, and tables of those (arrays or string-keyed objects). Functions, userdata, coroutines and
metatables are not transferable; a table that contains itself is refused (limits: nesting 32, 100,000
values, 4 MiB).

---

## 4. Events

Register with `on(event, handler)`; it returns an id for `off(id)`. Handlers of one event run in
registration order; across resources, in resource start order. Handlers receive plain-table snapshots.

```lua
local id = on("playerJoined", function(player) ... end)
off(id)            -- true if it existed
```

At most 256 handlers per resource. Event names are 1..64 of `A-Z a-z 0-9 _ : . -`.

### The player table

Every handler that receives a player gets this table:

| Field | Type | Meaning |
|---|---|---|
| `id` | integer | The player's id on this server. |
| `name` | string | The name the player joined with. |
| `ip` | string | Dotted IPv4 address. |
| `accountId` | string | **Placeholder identity.** Today the server derives it from the unauthenticated name (`name:alice`). Stable per name but **not proof of identity**; use it as a storage key, never as a credential. Real accounts will replace the value without changing the field. |
| `ping` | integer | Smoothed round-trip estimate in ms. |
| `isAdmin` | boolean | May use restricted commands. **Today the server never sets this** (there are no in-game admins yet). |
| `console` | boolean | `false` for players, `true` for the console source of a command. |

### Built-in events

| Event | Handler arguments | Notes |
|---|---|---|
| `playerConnecting` | `info`, `deferral` | A connection passed the server's own checks and is about to be accepted. `info = { name, ip, accountId }` (no `id` yet). Call `deferral.reject(reason)` to refuse it. |
| `playerJoined` | `player` | The player is in the player list (`players.get` finds them). |
| `playerLeft` | `player`, `reason` | `reason` is `"quit"`, `"timeout"` or `"kicked"`. The player is already gone from `players`. |
| `chatMessage` | `player`, `text` | A chat line that passed the server's flood and validity checks. **Cancellable** with `cancelEvent()`. `/commands` never arrive here. |
| `playerStateUpdate` | `player`, `state` | Opt-in, throttled. `state = { x, y, z, heading, vx, vy, vz }`: metres, degrees, metres per second. |
| `resourceStart` | `name` | A resource finished loading (including this one). |
| `resourceStop` | `name` | A resource is about to stop; its state is still alive. |

**playerConnecting.** `deferral.reject(reason)` refuses the connection with `reason` shown to the player:
control characters become spaces, at most 200 bytes of UTF-8; empty or invalid text becomes
`Connection refused by the server.` The **first** reject wins, across resources; later ones are ignored. An
error in a handler **does not reject** (fail-open, logged). `deferral.reject` only works while the handler
runs; calling it later is an error. There is no asynchronous deferral yet.

```lua
on("playerConnecting", function(info, deferral)
  if info.name:lower():find("admin", 1, true) then
    deferral.reject("Names containing 'admin' are not allowed.")
  end
end)
```

**chatMessage.** `cancelEvent()` stops the line being broadcast. Every resource still sees the message
(cancelling does not hide it from other handlers); calling `cancelEvent()` from any other event is an error.
An error in a chat handler never cancels the message.

**playerStateUpdate.** Listening makes the server track state updates; with no listener the cost is zero.
At most `stateUpdateHz` (default 4) events per player per second are delivered, always the **latest** state;
the ones in between are dropped. Updates of a player who left are dropped.

### Custom events

`emit(event, ...)` delivers an event to **every running resource, including the caller**, synchronously.
Arguments are copied into each receiver, so a handler can change its copy freely. Built-in event names
cannot be emitted by scripts. Nesting is limited to 8 levels.

```lua
-- resource A
on("shop:purchase", function(buyerId, item) log.info(buyerId, "bought", item.name) end)
-- resource B
emit("shop:purchase", 3, { name = "bandage", price = 10 })
```

---

## 5. Players and the server

All of these are global functions or tables; none yields.

### players

| Call | Returns |
|---|---|
| `players.list()` | Array of player tables ordered by id. |
| `players.get(id)` | The player table or `nil`. `id` is an integer, a player table, or a **name** (ASCII case-insensitive exact match). |
| `players.count()` | Number of joined players. |

### Chat and moderation

Functions that act on a player take a player table or an integer id. They return `true`, or
`nil, "reason"` when the server could not do it (for example `nil, "no player 99"`). A wrong argument type
or an out-of-range id is a Lua error.

| Call | Meaning |
|---|---|
| `sendChat(target, text)` | A server notice. `target` is a player, an id or the string `"all"`. `text` must be 1..200 bytes (longer is an error); the server validates it too. |
| `kick(player [, reason])` | Disconnects after telling the player `reason` (default `Kicked by the server.`; control characters become spaces, at most 200 bytes). |
| `ban(player [, reason])` | Bans the name and address like the console `ban`, then disconnects. |
| `banIp(ip [, reason])` | Bans an IPv4 address like the console `banip`. |
| `unban(target)` | Lifts bans matching a name or an address like the console `unban`. |
| `getPosition(player)` | `x, y, z, heading` of the last accepted state, or `nil, "no position yet"` / `nil, "no such player"`. |
| `setPosition(player, x, y, z [, heading])` | **Needs protocol support that does not exist yet.** The runtime and the call are in place; the server adapter returns `nil, "..."` until the protocol has a server-to-client teleport message and the client applies it. Coordinates must be finite numbers. |

`kick` may make the server call back into the runtime at once: the kicked player's `playerLeft` handlers run
**inside** your `kick()` call. Do not assume the player list is unchanged after kicking.

---

## 6. Commands

```lua
registerCommand(name, handler [, { restricted = false, help = "text" }])
```

`handler(source, args, raw)`:

* `source` is the player table of whoever ran it, or the console source
  `{ id = -1, name = "console", console = true, isAdmin = true, ... }`.
* `args` is an array of strings split on spaces and tabs (at most 32).
* `raw` is the text after the command name with the ends trimmed, spacing inside kept.

`reply(source, text)` answers the caller: to the player's chat, or to the console output. It returns `true`
or `nil, reason`; text over 200 bytes is cut at a character boundary.

Rules:

* Names are 1..32 of `A-Z a-z 0-9 _`, **case-insensitive** (stored lowercase). Players type `/name`; the
  console types `name`.
* **A command line is never broadcast as chat.** An unknown `/x` is answered `Unknown command: /x.` to
  the sender (while at least one resource is running).
* A name can be taken by only one resource; registering a taken name is an error naming the owner. The
  same resource registering a name again replaces its handler. At most 128 commands per resource.
* **Reserved**: `list kick say ban banip unban bans stats help quit exit stop start restart refresh
  resources` belong to the server and the runtime.
* `restricted = true` commands are refused **before your handler runs** unless the caller is an
  administrator: the player gets `You do not have permission to use /name.` and the refusal is logged. The
  console is always an administrator. Because the server does not set `isAdmin` for any player yet,
  **today only the console can run restricted commands**.
* A handler error is logged and the player is told `The command failed (see the server log).`

```lua
registerCommand("heal", function(src, args)
  reply(src, "Healed " .. (args[1] or src.name))
end, { restricted = true, help = "Heal a player" })
```

---

## 7. Timers

| Call | Meaning |
|---|---|
| `setTimeout(fn, ms)` | Runs `fn()` once after `ms` (0..86,400,000). Returns an integer id. |
| `setInterval(fn, ms)` | Runs `fn()` every `ms` (1..86,400,000). Returns an id. |
| `clearTimer(id)` | Cancels either kind. `true` if it existed, else `false`. Safe inside the callback. |

Timers are driven by the server tick; they fire in the first tick at or after their due time, **in due-time
order, equal times in creation order**. The resolution is the tick (20 Hz by default: 50 ms). A timer created
inside a callback waits for the next tick even with delay 0. After a stall an interval fires **once**, not
once per missed period. An error in a callback is logged; an interval keeps running. At most 256 timers per
resource; `ms` must be a whole number.

---

## 8. Storage, config and convars

### storage

A small persistent key-value store, private to the resource.

| Call | Returns |
|---|---|
| `storage.get(key [, default])` | The stored value, or `default` (nil if omitted). |
| `storage.set(key, value)` | `true`. `nil` as the value deletes the key. Raises an error for a value that is not plain data or over the quota. |
| `storage.delete(key)` | `true` if the key existed. |
| `storage.keys()` | Array of keys, sorted. |
| `storage.flush()` | `true`, or `nil, reason` if the write failed. |

Keys are at most 128 bytes. Values are booleans, finite numbers, UTF-8 strings, `json.null` and tables that
are arrays (keys `1..n`, no holes) or objects (string keys only), nested at most 64 deep; no functions, no
cycles. Binary data must be encoded (base64, hex) first. The whole store is limited to 4 MiB.

Persistence: stored as JSON in `data/<resource>/storage.json`.

* A change is written within `storageFlushMs` (2 s) of the first unwritten change, when the resource stops,
  when the server shuts down, and on `storage.flush()`.
* Writes are **atomic**: the new content goes to `storage.json.tmp`, is flushed to disk and renamed over the
  old file, so a crash leaves the old file or the new one, never half of either.
* A failed write (disk full) is logged and retried each interval; the values stay in memory.
* A damaged `storage.json` makes the resource **refuse to start** with an error naming the file; the file is
  never overwritten.

### config

`config.get(key [, default])` returns a value from `resource.json`'s `config` overlaid with `config.json`
(any JSON value; tables are fresh copies). `config.keys()` lists the keys, sorted. Config is read-only and is
re-read when the resource starts.

### getConvar

`getConvar(name [, default])` reads a read-only server setting, always as a string. The names the server
adapter is specified to provide are `name`, `port`, `maxPlayers`, `tickHz`, `gamemode`, `lang` and `url`
(the `server.json` keys). An unknown name returns `default`.

### resource

`resource.name`, `resource.version` and `resource.dependencies` (array) describe the running resource.

---

## 9. Logging and JSON

### log

`log.info(...)`, `log.warn(...)`, `log.error(...)` and `print(...)` (same as `log.info`) convert their
arguments with `tostring`, join them with tabs and write one line to the server log as `[resource] text`.
Control characters other than newline and tab become `?` (a script cannot inject terminal escapes). Each
resource has a token bucket of 100 lines refilling 100 per second; the excess is dropped and counted in one
`N log lines suppressed (rate limit)` warning. Lines are cut at 8000 bytes.

### json

| Call | Returns |
|---|---|
| `json.encode(value [, { pretty = true }])` | A JSON string. Raises an error for functions, userdata, NaN and infinity, cycles, tables that are neither array nor object, strings that are not valid UTF-8, or output over 16 MiB. |
| `json.decode(text)` | The value, or `nil, "message (at byte N)"`. |
| `json.null` | A sentinel for JSON `null` (Lua `nil` cannot live in a table). |

Encoding rules: a table with keys `1..n` and nothing else is an array; a table with only string keys is an
object with keys **sorted bytewise** (stable output); **an empty table encodes as `{}`**; a table that mixes
both is an error. Integers encode without a fraction (`3`), floats keep one (`3.0`, `-0.5`, `1e+300`).
Control characters are escaped.

Decoding is strict RFC 8259: no comments, no trailing commas, no single quotes, no leading zeros, no trailing
data, **no duplicate object keys**, valid UTF-8 only, nesting at most 64. A number with no fraction or
exponent decodes to a Lua integer (64-bit); anything else to a float.

---

## 10. Talking between resources

### Exports

```lua
-- resource "bank"
export("balance", function(accountId) return storage.get("bal:" .. accountId, 0) end)

-- any resource that runs after it
local gold = exports.bank.balance("name:alice")
```

`export(name, fn)` publishes `fn` (names 1..64 of `A-Z a-z 0-9 _`). `exports.<resource>.<name>(...)` calls
it. A call is **synchronous**, runs in the target's own Lua state under the target's own
[budget and memory cap](#11-limits-and-what-happens-when-a-script-misbehaves), and **copies** arguments in
and results out (up to 32 results): neither side can change the other's tables, and there are no shared
references or metatables. The proxy resolves at call time, so you can call a resource that starts later.

Failures are Lua errors in the caller, which `pcall` can catch: `resource 'x' is not running`, `resource 'x'
has no export 'y'`, an error raised by the exported function (with its message), an argument or result that
cannot be copied (function, cycle, too large), `exports are nested too deeply` (limit 8). The `exports`
table is read-only.

Declare `"dependencies": ["bank"]` so the provider has loaded before your scripts call it.

### Custom events

See [section 4](#custom-events).

---

## 11. Limits and what happens when a script misbehaves

Every entry into script code runs under a budget. The defaults (a server can change them):

| Limit | Default | When exceeded |
|---|---|---|
| VM instructions per entry | 10,000,000 | The entry is aborted (checked every 1000 instructions). |
| Wall-clock time per entry | 100 ms | The entry is aborted. |
| Pattern-matching steps per entry (`find`, `match`, `gmatch`, `gsub`) | 10,000,000 | `string.find` etc. raise `pattern matching budget exceeded` (a normal, catchable error). |
| Lua memory per resource | 64 MiB | The allocation fails: Lua raises `not enough memory` in the script, which can `pcall` it and carry on. |
| Consecutive aborted entries | 3 | The resource is stopped and reported `Failed`. |
| Timers / handlers / commands per resource | 256 / 256 / 128 | The registering call raises an error. |
| Log lines | 100 burst, 100/s | Dropped and counted. |
| Storage per resource | 4 MiB | `storage.set` raises an error. |

What an abort looks like: the entry stops where it is (state it already changed stays changed), one log
line names the resource, the entry and the reason, and the next event is delivered normally.

```
script: resource 'hog' aborted in handler for 'playerJoined': instruction budget exceeded (10000000 instructions)
(violation 1 in a row)
```

* **`pcall` cannot swallow an abort.** After the budget trips, the next instruction raises again, so
  `pcall(function() while true do end end)` does not return control to your code. The same holds for
  loops inside coroutines: `coroutine.resume` and `coroutine.wrap` cannot swallow an abort either.
* A violation counts only when entries are aborted **in a row**; one normal entry resets the count.
* Load time counts: a script that loops forever at top level fails the start.
* A call into another resource's export charges the *provider's* budget; the caller's budget keeps running
  too (the wall-clock time is shared).

What the budget does **not** bound, so you know: `table.sort` on a huge array and `string.rep` of a very large
result run as one native call that cannot be interrupted; they are bounded only by the memory cap
(64 MiB), which keeps them to tens of milliseconds.

---

## 12. A complete example

`resources/welcome` greets players and counts their visits (persistent storage), adds `/me`, and adds an
administrator-only `/tp`. This is the exact content of `libs/scripting/examples/welcome/`, and
`tests/scripting_test.cpp` runs it and checks the output and that this document contains both files
unchanged.

`resource.json`

```json
{
  "name": "welcome",
  "version": "1.0.0",
  "description": "Greets players, counts their visits, and adds /me and an admin-only /tp.",
  "author": "GAGE-MP",
  "server_scripts": ["main.lua"],
  "config": {
    "welcome": "Welcome to the server, {name}! Type /me <action> to emote.",
    "announceJoins": true
  }
}
```

`main.lua`

```lua
-- welcome: greets players, counts their visits, adds /me and an admin-only /tp.
-- This file is the example in docs/api/gamemode.md and runs in tests/scripting_test.cpp.

local greeting = config.get("welcome", "Welcome, {name}!")
local announce = config.get("announceJoins", true)

-- Chat lines are limited to 200 bytes; keep anything built from player input inside that.
local function clip(text)
  if #text > 190 then return text:sub(1, 190) end
  return text
end

-- {name} -> the player's name. A function replacement keeps '%' in names harmless.
local function fill(template, player)
  return (template:gsub("{name}", function() return player.name end))
end

on("playerJoined", function(player)
  -- Visits are kept per account id in this resource's storage (data/welcome/storage.json).
  local key = "visits:" .. player.accountId
  local visits = storage.get(key, 0) + 1
  storage.set(key, visits)

  sendChat(player, fill(greeting, player))
  if visits > 1 then
    sendChat(player, "This is visit number " .. visits .. ".")
  end
  if announce then
    sendChat("all", player.name .. " joined (" .. players.count() .. " online).")
  end
end)

on("playerLeft", function(player, reason)
  if announce then
    sendChat("all", player.name .. " left (" .. reason .. ").")
  end
end)

-- /me <action>: "* alice waves".
registerCommand("me", function(source, args, raw)
  if raw == "" then
    reply(source, "Usage: /me <action>")
    return
  end
  sendChat("all", clip("* " .. source.name .. " " .. raw))
end, { help = "Describe an action" })

-- /tp <x> <y> <z> or /tp <player>: admins only. The runtime refuses everyone else before this
-- function runs, so there is no permission check here.
registerCommand("tp", function(source, args)
  if source.console then
    reply(source, "/tp needs a player; use it from the game chat.")
    return
  end

  local x, y, z, heading
  if #args == 3 then
    x, y, z = tonumber(args[1]), tonumber(args[2]), tonumber(args[3])
    if not (x and y and z) then
      reply(source, "Usage: /tp <x> <y> <z> | /tp <player>")
      return
    end
    heading = 0
  elseif #args == 1 then
    -- A whole number is a player id; anything else is looked up as a name.
    local target = players.get(math.tointeger(tonumber(args[1])) or args[1])
    if not target then
      reply(source, "No such player: " .. args[1])
      return
    end
    local tx, ty, tz, th = getPosition(target)
    if not tx then
      reply(source, target.name .. " has no position yet.")
      return
    end
    x, y, z, heading = tx, ty, tz, th
  else
    reply(source, "Usage: /tp <x> <y> <z> | /tp <player>")
    return
  end

  local ok, err = setPosition(source, x, y, z, heading)
  if ok then
    reply(source, string.format("Teleported to %.1f, %.1f, %.1f.", x, y, z))
  else
    -- setPosition needs protocol support the client does not have yet; say so instead of failing.
    reply(source, "Teleport failed: " .. tostring(err))
  end
end, { restricted = true, help = "Teleport (admins)" })

log.info("welcome loaded, version " .. resource.version)
```

How it behaves:

| Who does what | Result |
|---|---|
| Alice joins | Alice sees `Welcome to the server, Alice! Type /me <action> to emote.`; everyone sees `Alice joined (1 online).` |
| Alice joins again after a restart | She also sees `This is visit number 2.` (kept in `data/welcome/storage.json`). |
| Alice types `/me waves` | Everyone sees `* Alice waves`. `/me` alone answers `Usage: /me <action>`. |
| Alice types `/tp 1 2 3` | `You do not have permission to use /tp.` The handler never runs. |
| An administrator types `/tp 10 20 30` | `Teleported to 10.0, 20.0, 30.0.` once the server supports `setPosition`; until then `Teleport failed: ...` and the reason. |
| An administrator types `/tp Alice` or `/tp 1` | Teleports to Alice's last position and heading, or `Alice has no position yet.` |
| Alice leaves | Everyone sees `Alice left (quit).` |

Change the greeting without touching the script by dropping a `config.json` next to it:
`{ "welcome": "Hello {name}!", "announceJoins": false }`.

---

## 13. Embedding the runtime (for server developers)

`libs/scripting/scripting.h` is the host interface; every public symbol has a doc comment. The shape: the
server owns a `scripting::ScriptRuntime` and an implementation of `scripting::IServerApi` (what scripts may
ask of the server), calls the runtime's `On*` methods when something happens, `Tick()` once per tick and
`ExecuteConsole()` for console lines. Nothing in the library depends on ENet or the GAGE protocol, so it is
tested with a fake `IServerApi`.

### Thread rules

The whole library is single-threaded: every `ScriptRuntime` method and every `IServerApi` callback runs on
**one** thread, the thread that calls `Server::Poll()`. No locks, no background threads. The runtime is
**re-entrant in one direction**: while a script runs, an `IServerApi` call such as `Kick()` may call back into
`OnPlayerLeft()`. So the server must call the runtime only where it holds no iterator or pointer into its
player tables across the call, and must re-check any `Conn` it keeps after the call returns (a handler may
have kicked that player).

### Build (CMake)

The top-level `project()` must list `C` (it already does: `project(GAGE_MP C CXX)`).

```cmake
# Lua 5.4.9 as C, with the pattern budget patch (third_party/lua/GAGE-PATCHES.md).
set(LUA_DIR ${GAGE_ROOT}/third_party/lua/src)
add_library(lua STATIC
  ${LUA_DIR}/lapi.c ${LUA_DIR}/lcode.c ${LUA_DIR}/lctype.c ${LUA_DIR}/ldebug.c ${LUA_DIR}/ldo.c
  ${LUA_DIR}/ldump.c ${LUA_DIR}/lfunc.c ${LUA_DIR}/lgc.c ${LUA_DIR}/llex.c ${LUA_DIR}/lmem.c
  ${LUA_DIR}/lobject.c ${LUA_DIR}/lopcodes.c ${LUA_DIR}/lparser.c ${LUA_DIR}/lstate.c
  ${LUA_DIR}/lstring.c ${LUA_DIR}/ltable.c ${LUA_DIR}/ltm.c ${LUA_DIR}/lundump.c ${LUA_DIR}/lvm.c
  ${LUA_DIR}/lzio.c ${LUA_DIR}/lauxlib.c ${LUA_DIR}/lbaselib.c ${LUA_DIR}/lcorolib.c
  ${LUA_DIR}/lmathlib.c ${LUA_DIR}/loslib.c ${LUA_DIR}/lstrlib.c ${LUA_DIR}/ltablib.c ${LUA_DIR}/lutf8lib.c)
target_compile_definitions(lua PRIVATE GAGE_LUA_PATTERN_BUDGET)
target_include_directories(lua PUBLIC ${LUA_DIR})
set_target_properties(lua PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)

set(SCRIPTING_DIR ${GAGE_ROOT}/libs/scripting)
add_library(gage_scripting STATIC
  ${SCRIPTING_DIR}/fsutil.cpp ${SCRIPTING_DIR}/value.cpp ${SCRIPTING_DIR}/storage.cpp
  ${SCRIPTING_DIR}/manifest.cpp ${SCRIPTING_DIR}/sandbox.cpp ${SCRIPTING_DIR}/luaapi.cpp
  ${SCRIPTING_DIR}/runtime.cpp)
target_include_directories(gage_scripting PUBLIC ${GAGE_ROOT}/libs)
target_compile_features(gage_scripting PUBLIC cxx_std_17)
target_compile_options(gage_scripting PRIVATE -fno-exceptions)
target_link_libraries(gage_scripting PUBLIC lua)

find_package(Threads REQUIRED)
add_executable(scripting_test ${GAGE_ROOT}/tests/scripting_test.cpp)
target_link_libraries(scripting_test PRIVATE gage_scripting Threads::Threads)
set_target_properties(scripting_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/selftest)
```

The test reads `libs/scripting/examples` and this document, so run it from the repository root (or pass the
root as `argv[1]`). Include as `#include "scripting/scripting.h"`.

### What the server provides: `IServerApi`

An adapter class holding a `Server&`:

| Method | Server side |
|---|---|
| `Log(level, line)` | `Server::Log` (the server adds the timestamp). |
| `NowMs()` | A monotonic millisecond clock. |
| `Players()`, `GetPlayer(id, out)` | A snapshot of `players_` as `PlayerInfo`. Fill `accountId` with `"name:" + name` for now, `isAdmin` with `false`. |
| `SendChat(to, text, err)`, `SendChatAll(text, err)` | `Notice(conn, text)` / `Say(text)`; return false with a reason when the text fails `proto::ValidChat` or the player is gone. |
| `Kick(id, reason, err)` | `Server::Kick`. **Today it sends the fixed notice `You were kicked from the server.`**; delivering `reason` needs a small change (a `Kick(id, reason)` overload that sends `Notice(reason)`). |
| `Ban`, `BanIp`, `Unban` | The existing `Server::Ban`, `BanIp`, `Unban`. |
| `SetPosition(id, pos, err)` | Return `false` with `"setPosition needs protocol support"` until the protocol has the message. |
| `GetConvar(name, out)` | From `Config`: `name`, `port`, `maxPlayers`, `tickHz`, `gamemode`, `lang`, `url`. |

### What the server calls

| Where (`apps/server/server.cpp`) | Call |
|---|---|
| `Server::Start`, after the socket is bound | construct the runtime; `rt.StartAll()` |
| `Server::Stop`, before the host is closed | `rt.Shutdown()` |
| `Server::Tick(nowUs)` | `rt.Tick()` once |
| Hello handling, after the `ServerFull` / `NameTaken` checks and before `Accept` | `ConnectVerdict v = rt.OnPlayerConnecting({name, address, accountId});` when `!v.accept`: `Refuse(c, proto::RefuseReason::Rejected, v.reason)`. **Needs a new `RefuseReason` value** (for example `Rejected = 9`) and the client's text for it. |
| End of `Server::Accept` | `rt.OnPlayerJoined(id)`; re-check `c` afterwards |
| `Server::RemovePlayer`, after the player left `players_` | `rt.OnPlayerLeft(snapshotTakenBeforeErase, reason)` with `Quit`/`Timeout`/`Kicked` mapped from `proto::LeaveReason` |
| `Server::OnChat`, after `ValidChat` and the flood check, before `SendToAll` | `if (!rt.OnChatMessage(c.id, text)) return;` and re-find the `Conn` if it is used afterwards |
| `Server::OnState`, after the state is accepted | `if (rt.WantsPlayerState()) rt.OnPlayerState(c.id, state);` |
| `Server::Execute`, **at the top** (the built-in `stop` quits the server whatever follows it) | `std::string out; auto r = rt.ExecuteConsole(line, out); if (r != ConsoleResult::NotHandled) { output = out; return r == ConsoleResult::Ok ? CommandResult::Ok : CommandResult::Error; }` |

`ExecuteConsole` handles `start`, `restart`, `refresh`, `resources`, `stop <resource>` and the commands
resources registered. `stop` with no argument is `NotHandled`, so the server's own `stop` still quits.

Options: `RuntimeOptions` (`resourcesDir`, `dataDir`, `limits`, `stateUpdateHz`, `storageFlushMs`,
`storageMaxBytes`, `maxTimersPerResource`, `maxHandlersPerResource`, `logBurst`, `logPerSecond`,
`atomicWrite`); `ScriptRuntime::Validate(options, &err)` rejects bad values with the field name, otherwise
out-of-range values are clamped.

---

## 14. What is not there yet

* **Not wired into `gage-server`** (section 13 lists the calls).
* **`setPosition` cannot teleport**: it needs a server-to-client message and a client that applies it.
* **`kick` cannot show your reason** until the server's `Kick` accepts one.
* **No administrators**: `isAdmin` is false for every player, so `restricted` commands are console-only.
* **`playerConnecting` has no async deferral** (no `deferral.defer()` / `update()` for checking a web service
  before accepting); handlers must decide synchronously.
* **No `wait()` / `createThread`**: use timers and coroutines you resume yourself.
* **No client scripts**: `client_files` is validated and ignored.
* `table.sort` and huge `string.rep` results are bounded by the memory cap, not by the budget (section 11).
* The Linux build of the runtime is not exercised yet (it uses only ISO C and C++17 and `std::filesystem`).
* `third_party/lua/src/lstrlib.c` carries one local patch (the pattern budget). Re-apply
  `third_party/lua/patches/lstrlib-pattern-budget.patch` when updating Lua.
