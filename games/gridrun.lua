#!/bin/lua
-- SPDX-License-Identifier: GPL-2.0-only
-- gridrun: a light-cycle duel on a dead city's power grid (Zenix's games package; docs/man/gridrun.txt).
-- You ride the cyan trail, a daemon of the grid rides the magenta one; whoever hits a wall or a trail derezzes.
-- Arrow keys turn, space starts the next round, p pauses, q or Escape (or the close gadget) quits.
-- Everything in it is made here: the pictures from strings, the sounds and the music (a 4-channel module) from
-- numbers. It is also the example for the zenix module (docs/man/zenix-lua.txt).
local z = require "zenix"

-- the grid: 64 x 42 cells of 8 x 4 pixels (square on the screen: a hires pixel is half as wide as it is tall)
local CW, CH, GW, GH = 8, 4, 64, 42
local W, H = GW * CW, GH * CH + 14              -- and a status line under it
local STEP = 70                                 -- ms a cell
local seed = tonumber(os.getenv("GRIDRUN_SEED") or "") or os.time()
local LOG = os.getenv("GRIDRUN_LOG")            -- the tests: a line on standard output for what happens
math.randomseed(seed)
local function log(...) if LOG then print("gridrun:", ...) io.stdout:flush() end end

local win, why = z.window("gridrun", W, H)
if not win then io.stderr:write("gridrun: ", why, "\n") os.exit(1) end

-- the theme's pens nearest the colours wanted: the screen's palette belongs to the desktop
local BG, GRID, GRID2 = z.pen(0x001), z.pen(0x123), z.pen(0x234)
local YOU, DAEMON, TEXT, HOT = z.pen(0x4CE), z.pen(0xC4C), z.pen(0xCCC), z.pen(0xE52)
log("pens", BG, GRID, YOU, DAEMON, TEXT, HOT)

-- ---- the pictures: a spark where a cycle derezzes, the cycles' heads ----
local function burst(c1, c2)
  local rows, a, b = {}, string.format("%x", c1), string.format("%x", c2)
  for y = 0, 15 do
    local r = {}
    for x = 0, 31 do
      local dx, dy = x - 15, 2 * y - 15         -- in half-pixels across, so the burst is round on the screen
      local d2 = dx * dx + dy * dy
      local adx, ady = math.abs(dx), math.abs(dy)
      local ray = adx < 2 or ady < 2 or math.abs(adx - ady) < 2
      if d2 < 30 then r[#r + 1] = b
      elseif ray and d2 < 240 then r[#r + 1] = d2 < 110 and a or b
      else r[#r + 1] = "." end
    end
    rows[#rows + 1] = table.concat(r)
  end
  return z.image(rows)
end
local spark = { [1] = burst(YOU, TEXT), [2] = burst(DAEMON, HOT) }
local function cycle(edge, core)
  local e, c = string.format("%x", edge), string.format("%x", core)
  return z.image({ e:rep(8), e .. c:rep(6) .. e, e .. c:rep(6) .. e, e:rep(8) })
end
local head = { cycle(TEXT, YOU), cycle(HOT, DAEMON) }

-- ---- the sounds: 8-bit signed bytes, made with whole numbers only (a 68020 has no FPU: every float is slow) ----
local SIN = {}                                  -- one turn of a sine in 64 steps, -127 .. 127
for i = 0, 63 do SIN[i] = math.floor(math.sin(i * math.pi / 32) * 127 + 0.5) end
local function bytes(n, f)
  local t = {}
  for i = 0, n - 1 do
    local v = f(i)
    if v > 127 then v = 127 elseif v < -128 then v = -128 end
    t[i + 1] = string.char(v & 255)
  end
  return table.concat(t)
end
local turnfx = z.sample(bytes(600, function(i) return ((i // 6) % 2 == 0 and 60 or -60) * (600 - i) // 600 end), 11025)
local crashfx = z.sample(bytes(5000, function(i)
  local k = 5000 - i
  return math.random(-120, 120) * k // 5000 * k // 5000 end), 8000)
local startfx = z.sample(bytes(2400, function(i)
  local f = i < 800 and 3 or i < 1600 and 4 or 6
  return SIN[(i * f) % 64] * 90 // 127 * (900 - i % 800) // 900 end), 11025)

-- ---- the music: a ProTracker module written here (M.K., 4 channels): a dark loop in A minor ----
local function mod()
  local function be16(v) return string.char((v >> 8) & 255, v & 255) end
  local smp = {
    { name = "bass", data = bytes(64, function(i) return (i < 32 and i * 7 - 110 or (63 - i) * 7 - 110) end), loop = true },
    { name = "pad", data = bytes(32, function(i) return (SIN[i * 2] * 70 + SIN[(i * 4) % 64] * 30) // 127 end), loop = true },
    { name = "kick", data = (function()
        local ph = 0
        return bytes(1600, function(i) ph = ph + 640 - i * 2 // 5; return SIN[(ph >> 8) % 64] * (1600 - i) // 1600 end)
      end)() },
    { name = "hat", data = bytes(400, function(i) local k = 400 - i; return math.random(-70, 70) * k // 400 * k // 400 * k // 400 end) },
  }
  local per = { ["A-1"] = 508, ["C-2"] = 428, ["D-2"] = 381, ["E-2"] = 339, ["F-2"] = 320, ["G-2"] = 285,
                ["A-2"] = 254, ["C-3"] = 214, ["E-3"] = 170, ["G-1"] = 570, ["F-1"] = 640, ["E-1"] = 678 }
  local function note(p, s, eff, prm)
    p, s = p and per[p] or 0, s or 0
    eff, prm = eff or 0, prm or 0
    return string.char(((s & 0x10)) | ((p >> 8) & 15), p & 255, ((s & 15) << 4) | eff, prm)
  end
  local bassline = { "A-1", "A-1", "C-2", "A-1", "F-1", "F-1", "G-1", "E-1" }
  local pads = { "A-2", "C-3", "F-2", "E-2" }
  local pats = {}
  for p = 0, 1 do
    local rows = {}
    for r = 0, 63 do
      local b = (r % 8 == 0 or r % 8 == 3 or r % 8 == 6) and note(bassline[(r // 8) % 8 + 1], 1, 0xC, 40) or note()
      local k = (r % 8 == 0) and note("C-2", 3, 0xC, 64) or (p == 1 and r % 16 == 12) and note("C-2", 3, 0xC, 40) or note()
      local hh = (r % 4 == 2) and note("C-3", 4, 0xC, r % 8 == 6 and 28 or 16) or note()
      local pd = (p == 1 and r % 16 == 0) and note(pads[r // 16 + 1], 2, 0xC, 20) or note()
      rows[#rows + 1] = b .. k .. hh .. pd
    end
    pats[#pats + 1] = table.concat(rows)
  end
  local hdr = { ("gridrun"):rep(1) .. string.rep("\0", 13) }
  for i = 1, 31 do
    local s = smp[i]
    if s then
      local n = #s.data // 2
      hdr[#hdr + 1] = s.name .. string.rep("\0", 22 - #s.name) .. be16(n) .. "\0\64" ..
        (s.loop and be16(0) .. be16(n) or be16(0) .. be16(1))
    else
      hdr[#hdr + 1] = string.rep("\0", 22) .. be16(0) .. "\0\0" .. be16(0) .. be16(1)
    end
  end
  local order = { 0, 0, 1, 1 }
  local ord = {}
  for i = 1, 128 do ord[i] = string.char(order[i] or 0) end
  hdr[#hdr + 1] = string.char(#order, 127) .. table.concat(ord) .. "M.K."
  local data = {}
  for _, s in ipairs(smp) do data[#data + 1] = s.data end
  return table.concat(hdr) .. table.concat(pats) .. table.concat(data)
end
local music = mod()

-- ---- the game ----
local grid, cyc, score, state, paused = {}, {}, { 0, 0 }, "ready", false
local DIRS = { up = { 0, -1 }, down = { 0, 1 }, left = { -1, 0 }, right = { 1, 0 } }
local function at(x, y) return grid[y * GW + x] end
local function cell(x, y, p) win:fill(x * CW, y * CH, CW, CH, p) end
local function status(msg, pen)
  win:fill(0, GH * CH, W, H - GH * CH, BG)
  win:text(8, GH * CH + 4, string.format("YOU %d", score[1]), YOU, BG)
  win:text(W - 8 - 8 * 10, GH * CH + 4, string.format("%3d DAEMON", score[2]), DAEMON, BG)
  if msg then win:text((W - 8 * #msg) // 2 // 8 * 8, GH * CH + 4, msg, pen or TEXT, BG) end
end
local function board()
  win:clear(BG)
  for x = 0, GW - 1, 4 do win:line(x * CW, 0, x * CW, GH * CH - 1, x % 16 == 0 and GRID2 or GRID) end
  for y = 0, GH - 1, 7 do win:line(0, y * CH, W - 1, y * CH, y % 14 == 0 and GRID2 or GRID) end
  win:fill(0, 0, W, CH, GRID2); win:fill(0, (GH - 1) * CH, W, CH, GRID2)
  win:fill(0, 0, CW, GH * CH, GRID2); win:fill(W - CW, 0, CW, GH * CH, GRID2)
end
local function newround()
  grid = {}
  for x = 0, GW - 1 do grid[x] = true; grid[(GH - 1) * GW + x] = true end
  for y = 0, GH - 1 do grid[y * GW] = true; grid[y * GW + GW - 1] = true end
  cyc = { { x = 10, y = GH // 2, d = DIRS.right, pen = YOU, alive = true },
          { x = GW - 11, y = GH // 2, d = DIRS.left, pen = DAEMON, alive = true } }
  board()
  for i, c in ipairs(cyc) do grid[c.y * GW + c.x] = i; win:blit(head[i], c.x * CW, c.y * CH) end
  status("SPACE: RIDE", TEXT)
  state = "ready"
  win:flush()
end
-- the daemon: straight on while the way ahead is clear for a while, else the turn with the longest free run
local function run(x, y, d)
  local n = 0
  repeat x, y, n = x + d[1], y + d[2], n + 1 until n > 40 or at(x, y)
  return n - 1
end
local function think(c)
  local ahead = run(c.x, c.y, c.d)
  if ahead > 3 and math.random(100) > 4 then return end
  local best, bestn = c.d, ahead * 4
  for _, k in ipairs({ "up", "down", "left", "right" }) do
    local d = DIRS[k]
    if d[1] ~= -c.d[1] or d[2] ~= -c.d[2] then
      local n = run(c.x, c.y, d) * 4 + math.random(0, 7)
      if n > bestn then best, bestn = d, n end
    end
  end
  c.d = best
end
local function step()
  think(cyc[2])
  local moved = {}
  for i, c in ipairs(cyc) do
    cell(c.x, c.y, c.pen)               -- the old head becomes trail
    c.x, c.y = c.x + c.d[1], c.y + c.d[2]
    moved[i] = c.y * GW + c.x
  end
  for i, c in ipairs(cyc) do
    if at(c.x, c.y) or moved[1] == moved[2] then c.alive = false end
  end
  for i, c in ipairs(cyc) do
    if c.alive then grid[moved[i]] = i; win:blit(head[i], c.x * CW, c.y * CH) end
  end
  if cyc[1].alive and cyc[2].alive then return end
  for i, c in ipairs(cyc) do
    if not c.alive then win:blit(spark[i], c.x * CW - 12, c.y * CH - 6) end
  end
  z.play(crashfx, 64, 3)
  local msg, pen
  if not cyc[1].alive and not cyc[2].alive then msg, pen = "BOTH DEREZZED", HOT
  elseif not cyc[1].alive then score[2] = score[2] + 1; msg, pen = "DEREZZED", DAEMON
  else score[1] = score[1] + 1; msg, pen = "DAEMON PURGED", YOU end
  log("round", msg, score[1], score[2])
  status(msg, pen)
  win:title(string.format("gridrun  %d : %d", score[1], score[2]))
  state = "over"
end

newround()
local ok, mwhy = z.music(music, 40)
log("music", ok and "on" or mwhy)
local nextt = z.time()
while true do
  local ev = z.event(state == "ride" and not paused and math.max(0, nextt - z.time()) or nil)
  if ev then
    if ev.type == "close" then break end
    if ev.type == "key" and not ev.up then
      local k = ev.key
      log("key", k)
      if k == "q" or k == "escape" then break
      elseif k == "p" and state == "ride" then
        paused = not paused
        status(paused and "PAUSED" or nil)
        nextt = z.time() + STEP
      elseif k == "space" and state ~= "ride" then
        if state == "over" then newround() end
        z.play(startfx, 48, 2)
        status()
        state, paused, nextt = "ride", false, z.time() + STEP
      elseif DIRS[k] and state == "ride" then
        local d, c = DIRS[k], cyc[1]
        if d[1] ~= -c.d[1] or d[2] ~= -c.d[2] then
          if d ~= c.d then z.play(turnfx, 28, 1) end
          c.d = d
        end
      end
    elseif ev.type == "expose" or ev.type == "resize" then
      board()
      for y = 0, GH - 1 do
        for x = 0, GW - 1 do
          local o = grid[y * GW + x]
          if o == 1 or o == 2 then cell(x, y, cyc[o].pen) end
        end
      end
      status(state == "over" and "PRESS SPACE" or nil)
    end
  end
  if state == "ride" and not paused and z.time() >= nextt then
    step()
    nextt = nextt + STEP
    if z.time() > nextt + 4 * STEP then nextt = z.time() + STEP end   -- far behind: no rush of steps
  end
  win:flush()
end
z.music(nil)
win:close()
log("bye", score[1], score[2])
