-- SPDX-License-Identifier: GPL-2.0-only
-- A stand-in for Lua's zenix module on the PC (tools/games-host-test.sh): the same calls (docs/man/zenix-lua.txt),
-- each argument checked as lzenix.c checks it, nothing drawn or played. The time is the stand-in's own: a wait moves
-- it on at once. The events come from ZSTUB_KEYS ("MS:KEY,MS:KEY,...": a key going down and up at that time, "close"
-- the close gadget); with no event left, a wait without a limit ends the run with an error. At the end (the Lua
-- state closing) it prints what was asked of it on standard error, and ZSTUB_MOD=FILE keeps the last module given to
-- music for the replayer's check.
local S = { t = 0, fills = 0, lines = 0, texts = 0, blits = 0, images = 0, samples = 0, plays = 0, music = 0,
            flushes = 0, keys = 0, oob = 0 }
local PAL = { [0] = 0x344, 0x112, 0xEEE, 0x223, 0x47B, 0xBBC, 0x455, 0xFFF, 0xCCD, 0xD54, 0x7C5, 0xEB4, 0x69E,
              0xB7C, 0x5BC, 0x567 }          -- the flat theme's palette
local queue = {}
for at, k in (os.getenv("ZSTUB_KEYS") or ""):gmatch("(%d+):([%w]+)") do
  at = tonumber(at)
  if k == "close" then queue[#queue + 1] = { at = at, ev = { type = "close" } }
  else
    queue[#queue + 1] = { at = at, ev = { type = "key", key = k, char = #k == 1 and k or "", up = false, code = 0, qual = 0 } }
    queue[#queue + 1] = { at = at + 30, ev = { type = "key", key = k, char = "", up = true, code = 0, qual = 0 } }
  end
end
table.sort(queue, function(a, b) return a.at < b.at end)

local function int(v, what)
  if math.type(v) ~= "integer" then error("zstub: " .. what .. " is not an integer: " .. tostring(v), 3) end
  return v
end
local function pen(v) v = int(v, "pen") if v < 0 or v > 15 then error("zstub: pen " .. v, 3) end end
local W = {}
W.__index = W
function W:fill(x, y, w, h, p)
  int(x, "x") int(y, "y") int(w, "w") int(h, "h") pen(p)
  if x < 0 or y < 0 or x + w > self.w or y + h > self.h then S.oob = S.oob + 1 end
  S.fills = S.fills + 1
end
function W:clear(p) if p ~= nil then pen(p) end S.fills = S.fills + 1 end
function W:line(x0, y0, x1, y1, p)
  int(x0, "x0") int(y0, "y0") int(x1, "x1") int(y1, "y1") pen(p)
  if math.min(x0, x1) < 0 or math.min(y0, y1) < 0 or math.max(x0, x1) >= self.w or math.max(y0, y1) >= self.h then
    S.oob = S.oob + 1
  end
  S.lines = S.lines + 1
end
function W:text(x, y, s, fg, bg)
  int(x, "x") int(y, "y") assert(type(s) == "string") pen(fg) if bg ~= nil then pen(bg) end
  if x < 0 or y < 0 or x + 8 * #s > self.w or y + 8 > self.h then S.oob = S.oob + 1 end
  S.texts = S.texts + 1
end
function W:blit(img, x, y)
  assert(getmetatable(img) == "zstub.image", "zstub: blit of a non-picture")
  int(x, "x") int(y, "y")
  S.blits = S.blits + 1
end
function W:flush() S.flushes = S.flushes + 1 end
function W:size() return self.w, self.h end
function W:title(t) assert(type(t) == "string") self.t = t end
function W:close() self.closed = true end

local z = {}
function z.window(title, w, h)
  assert(type(title) == "string") int(w, "width") int(h, "height")
  if w < 8 or w > 2048 or h < 8 or h > 2048 then error("zstub: width and height 8 .. 2048", 2) end
  if w > 640 or h > 230 then error(("zstub: a %dx%d window does not fit the 640x256 screen with its frame"):format(w, h), 2) end
  return setmetatable({ w = w, h = h }, W)
end
function z.image(rows)
  assert(type(rows) == "table", "zstub: image of a non-table")
  local w = #rows[1]
  for i, r in ipairs(rows) do
    if #r ~= w then error("zstub: image line " .. i .. " is " .. #r .. " characters, not " .. w, 2) end
    if r:find("[^0-9a-fA-F. ]") then error("zstub: image line " .. i .. ": " .. r, 2) end
  end
  S.images = S.images + 1
  return setmetatable({}, { __metatable = "zstub.image" })
end
function z.palette() local t = {} for i = 0, 15 do t[i] = PAL[i] end return t end
function z.pen(c)
  int(c, "colour")
  local best, bd = 0, math.huge
  for i = 0, 15 do
    local p = PAL[i]
    local dr, dg, db = (p >> 8 & 15) - (c >> 8 & 15), (p >> 4 & 15) - (c >> 4 & 15), (p & 15) - (c & 15)
    local d = 3 * dr * dr + 4 * dg * dg + 2 * db * db
    if d < bd then best, bd = i, d end
  end
  return best
end
function z.screen() return 640, 256, 4 end
function z.flush() end
function z.sample(d, rate)
  assert(type(d) == "string" and #d >= 2 and #d <= 131070, "zstub: sample size")
  rate = rate or 8000
  int(rate, "rate")
  assert(rate >= 1000 and rate <= 28000, "zstub: rate")
  S.samples = S.samples + 1
  return setmetatable({}, { __metatable = "zstub.sample" })
end
function z.play(s, vol, ch)
  assert(getmetatable(s) == "zstub.sample", "zstub: play of a non-sample")
  if vol ~= nil then int(vol, "volume") assert(vol >= 0 and vol <= 64) end
  if ch ~= nil then int(ch, "channel") assert(ch >= 0 and ch <= 3) end
  S.plays = S.plays + 1
  return true
end
function z.music(d, vol)
  if d == nil then return end
  assert(type(d) == "string", "zstub: music of a non-string")
  if d:sub(1081, 1084) ~= "M.K." then return false, "zstub: not a 4-channel module" end
  S.music = #d
  local f = os.getenv("ZSTUB_MOD")
  if f then local o = assert(io.open(f, "wb")) o:write(d) o:close() end
  return true
end
function z.time() return S.t end
function z.event(ms)
  local q = queue[1]
  if q and (ms == nil or q.at <= S.t + ms) then
    table.remove(queue, 1)
    if q.at > S.t then S.t = q.at end
    if q.ev.type == "key" and not q.ev.up then S.keys = S.keys + 1 end
    return q.ev
  end
  if ms == nil then error("zstub: a wait without a limit and no event left", 2) end
  S.t = S.t + ms
  return nil
end
function z.sleep(ms) S.t = S.t + int(ms, "ms") end
setmetatable(S, { __gc = function()
  io.stderr:write(("zstub: fills %d lines %d texts %d blits %d images %d samples %d plays %d music %d flushes %d keys %d outside %d time %d\n")
    :format(S.fills, S.lines, S.texts, S.blits, S.images, S.samples, S.plays, S.music, S.flushes, S.keys, S.oob, S.t))
end })
package.preload.zenix = function() return z end
