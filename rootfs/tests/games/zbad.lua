-- SPDX-License-Identifier: GPL-2.0-only
-- Lua's zenix module with bad arguments (tools/luagame-test.sh, before the desktop runs: nothing here opens the
-- display or the sound server): every call must end in a Lua error or a false answer with a reason, never a crash.
-- A line "ok NAME: message" for each, "BAD NAME" when a call went through that should not have.
local z = require "zenix"
local function bad(name, f, ...)
  local ok, e = pcall(f, ...)
  if ok then print("BAD " .. name) else print("ok " .. name .. ": " .. tostring(e)) end
end
bad("sample too short", z.sample, "x")
bad("sample rate", z.sample, "abcd", 50)
bad("sample not a string", z.sample, {})
bad("play a number", z.play, 7)
bad("play volume", z.play, z.sample("abcd"), 99)
bad("window width", z.window, "w", 2, 50)
bad("image not a table", z.image, "x")
bad("event without a window", z.event)
local ok, why = z.music("not a module at all")
print((ok == false and why) and "ok music refused: " .. why or "BAD music")
local t0 = z.time()
z.sleep(100)
local t1 = z.time()
print(math.type(t0) == "integer" and t1 - t0 >= 100 and "ok time " .. (t1 - t0) or "BAD time " .. t0 .. " " .. t1)
print(z.event(0) == nil and "ok event(0) without a window: nil" or "BAD event(0)")
