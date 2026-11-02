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
