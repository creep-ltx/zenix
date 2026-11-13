-- SPDX-License-Identifier: GPL-2.0-only
-- Lua's C stack on Zenix (DESIGN D52 section 9, "The C stack"): every way a script can make the interpreter recurse
-- in C, each driven to Lua's own limit (LUAI_MAXCCALLS nested C calls or parser levels), where Lua must stop it with
-- an error and go on, never by running off the end of the program's fixed stack (no MMU: that would write into
-- the memory below it). Each line says which recursion and the error Lua gave (its first words), and "ok".
-- Argument 1 picks one case by name (to run them one at a time); none runs them all.
local only = arg[1]
local function short(e)
    e = tostring(e)
    if e:find("C stack overflow", 1, true) then return "C stack overflow" end
    e = e:gsub("^[^:]*:%d+: ", ""); return (e:match("^[^\n]*"):sub(1, 40))
end
local cases = {}
local order = {}
local function case(name, f) cases[name] = f; order[#order + 1] = name end

-- string.gsub calling a Lua function that calls gsub again: str_gsub's frame (with its buffer) on every level
case("gsub", function() local function f(s) return (string.gsub("x", "x", f)) end; return f("x") end)
-- string.format("%s") calling __tostring, which formats again
case("format", function()
    local mt = {}
    mt.__tostring = function(t) return string.format("%s", setmetatable({}, mt)) end
    return string.format("%s", setmetatable({}, mt))
end)
-- pcall inside pcall: a protected call's set-up on every level
case("pcall", function() local function p() local ok, e = pcall(p); if not ok then error(e, 0) end end; return p() end)
-- table.sort's comparison calling sort again
case("sort", function()
    local function cmp(a, b) table.sort({3, 2, 1}, cmp); return a < b end
    table.sort({3, 2, 1}, cmp)
end)
-- __index as a function that reads another table with the same kind of __index
case("index", function()
    local mt = {}
    mt.__index = function(t, k) return setmetatable({}, mt)[k] end
    return setmetatable({}, mt).x
end)
-- __add as a function that adds again
case("arith", function()
    local mt = {}
    mt.__add = function(a, b) return setmetatable({}, mt) + 1 end
    return setmetatable({}, mt) + 1
end)
-- a coroutine resuming a new coroutine on every level
case("resume", function()
    local function r() local co = coroutine.wrap(r); return co() end
    return r()
end)
-- load with a reader function that loads again (the parser's frames under the reader's)
case("load", function()
    local err
    local function reader() local f, e = load(reader); err = err or e; return nil end
    load(reader)
    error(err, 0)
end)
-- require of a module whose loader requires itself again under another name
case("require", function()
    local n = 0
    package.preload.zz = nil
    local function loader() n = n + 1; package.preload["m" .. n] = loader; return require("m" .. n) end
    package.preload.m0 = loader
    return require("m0")
end)
-- tostring calling __tostring calling tostring
case("tostring", function()
    local mt = {}
    mt.__tostring = function() return tostring(setmetatable({}, mt)) end
    return tostring(setmetatable({}, mt))
end)
-- the parser: brackets inside brackets, tables inside tables, functions inside functions, blocks inside blocks
case("parens", function() return assert(load("return " .. ("("):rep(400) .. "1" .. (")"):rep(400))) end)
case("tables", function() return assert(load("return " .. ("{"):rep(400) .. ("}"):rep(400))) end)
case("functions", function() return assert(load(("function f() "):rep(400) .. ("end "):rep(400))) end)
case("blocks", function() return assert(load(("do "):rep(400) .. ("end "):rep(400))) end)
case("ifs", function() return assert(load(("if x then "):rep(400) .. ("end "):rep(400))) end)
-- an error raised at the deepest level, with a traceback made there (lua.c's own message handler does that)
case("traceback", function()
    local function f(s) return (string.gsub("x", "x", f)) end
    local ok, e = xpcall(f, debug.traceback, "x")
    error(e, 0)
end)
-- string.rep, string.format and table.concat on big strings are not recursion, only memory: not here

for _, name in ipairs(order) do
    if not only or only == name then
        local ok, e = pcall(cases[name])
        print(name, ok and "no error" or short(e), "ok")
    end
end
print("cstack done")
