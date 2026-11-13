-- SPDX-License-Identifier: GPL-2.0-only
-- Lua on Zenix (DESIGN D52 section 9): one script run by tools/lua-host-test.sh with the PC's build of the same
-- pinned Lua 5.4.9 and by tools/lua-test.sh on Zenix in FS-UAE; the two outputs must be the same, line for line.
-- So it prints only what does not depend on the machine (no addresses, no times, no paths of the PC), and the
-- floating-point results that the C library's maths computes are printed to 12 digits (picolibc and the PC's libm may
-- differ in the last bit of a sine). Run from its own folder (require finds ./luamod.lua). Argument 1 is a folder to
-- write scratch files in.
local tmp = arg[1] or "/tmp"
local function say(...) print(...) end
local function try(f, ...) local ok, e = pcall(f, ...); return ok and "ok" or tostring(e):gsub("^[^:]*:%d+: ", "") end

say("== version", _VERSION, math.type(1), math.type(1.0), math.type("1"))

say("== integers")
say(math.maxinteger, math.mininteger, math.maxinteger + 1 == math.mininteger)
say(7 // 2, -7 // 2, 7 % -3, -7 % 3, 7.5 // 2, 2^10, 2^53 == 2^53 + 1)
say(0x7fffffff * 3, 123456789 * 987654321, 1 << 62, 1 << 63, 1 << 64, -1 >> 1, 5 & 3, 5 | 3, 5 ~ 3, ~0)
say(try(function() return 1 // 0 end), try(function() return 1 % 0 end))
say(math.tointeger(3.0), math.tointeger(3.5), 3 == 3.0, math.ult(1, -1), string.format("%d", -0))
say(tonumber("0x10"), tonumber("10", 2), tonumber("zz", 36), tonumber(" 12 "), tonumber("1e2"), tonumber("x"))

say("== floats")
say(0.1 + 0.2, 1 / 3, 1e100, -1e-300 * 1e-300, 1 / 0, -1 / 0, 0 / 0 ~= 0 / 0, 2^-1022, 1.7976931348623157e308)
say(3.14159, 100.0, -0.0, 1e15, 1e16, 123456789012345.0, 2^63, math.pi)
say(string.format("%.3f %5.1f %e %g %g %g %.14g", 1 / 3, 2.25, 12345.678, 1e20, 0.0001, 100000, 2 / 3))
say(string.format("%5.2f|%-8.3g|%+d|%x|%X|%o|%c|%q|%s|%10s|%-5s|", 3.14159, 0.000123456, 5, 255, 255, 8, 65,
    "a\nb\"c\0", nil, "right", "l"))
say(math.floor(-3.5), math.ceil(-3.5), math.fmod(7, 3), math.fmod(-7, 3), math.abs(-4), math.max(3, 9, 1),
    math.min(2.5, -1))
local function g12(x) return string.format("%.12g", x) end
say(g12(math.sqrt(2)), g12(math.sin(1)), g12(math.cos(1)), g12(math.tan(0.5)), g12(math.exp(1)), g12(math.log(10)),
    g12(math.log(8, 2)), g12(math.atan(1, 2)), g12(math.asin(0.5)), g12(10^0.5), g12(math.fmod(5.5, 2)))
say(string.format("%.14g", 1e300 * 1e10), math.huge, -math.huge, math.floor(2.5 + 0.5))
math.randomseed(42); local r = math.random(1, 100); say(r >= 1 and r <= 100, math.type(math.random()))

say("== strings")
local s = "Hello, Zenix on the Amiga!"
say(#s, s:upper(), s:lower(), s:sub(8, 12), s:sub(-6), s:rep(2, "|"), s:reverse():sub(1, 5), s:byte(1, 3))
say(string.char(72, 105), ("x"):rep(0), ("%d items"):format(3), s:find("Zenix"), s:find("o", 6, true))
say(s:match("(%a+), (%a+)"), s:gsub("%a+", string.upper, 2), ("a,b,,c"):gsub(",", ";"))
local words = {}
for w in s:gmatch("%a+") do words[#words + 1] = w end
say(table.concat(words, "/"), ("THE (quick) fox"):find("%((%a+)%)"), ("[[x]]"):match("%b[]"))
say(("hello world from lua"):gsub("(%w+) (%w+)", "%2 %1"), ("abc"):gsub("", "-"), ("  trim  "):match("^%s*(.-)%s*$"))
say(("THE (quick) fox"):find("%f[%a]%a+%f[%A]", 5), ("key=val; k2=v2"):gsub("(%w+)=(%w+)", "%2=%1"))
say(utf8.char(72, 228, 8364, 128512), utf8.len("häx€"), #utf8.char(8364), utf8.codepoint("€"))
for p, c in utf8.codes("aä€") do io.write(p, ":", c, " ") end print()
say(string.pack(">I2i4", 258, -2):byte(1, -1))
say(string.unpack("<i4", string.pack("<i4", -123456)), string.packsize("i4i8d"), #string.pack("z", "abc"))
say(("%s=%s"):format("k", 1.5), tostring(nil), tostring(true), tostring(12), tostring(1.25), 10 .. 20)
say(("x"):rep(100000):len(), ("ab"):rep(3, ","), string.format("%-3d|%03d|%.2s", 7, 7, "abcdef"))

say("== tables")
local t = {5, 3, 8, 1, 9, 2}
table.sort(t); say(table.concat(t, " "))
table.sort(t, function(a, b) return a > b end); say(table.concat(t, " "))
table.insert(t, 1, 0); table.insert(t, 7); say(table.remove(t), table.remove(t, 1), #t, table.concat(t, ","))
say(table.unpack({1, 2, 3})); say(select("#", nil, nil), select(2, "a", "b", "c"), select(-1, 1, 2, 3))
local m = table.move({1, 2, 3}, 1, 3, 2, {9}); say(table.concat(m, ","), #m, next({}), rawlen({1, 2}), rawequal(t, t))
local names = {}
for i = 1, 200 do names[i] = string.format("n%03d", (i * 37) % 200) end
table.sort(names); say(names[1], names[100], names[200], #names)
local keys = {}
for k in pairs({x = 1, y = 2, z = 3}) do keys[#keys + 1] = k end
table.sort(keys); say(table.concat(keys))
local big = {}
for i = 1, 20000 do big[i] = i * 2 end
local sum = 0
for _, v in ipairs(big) do sum = sum + v end
say(#big, sum, big[20000])
say(try(table.sort, {3, 1, 2}, function(a, b) return true end))

say("== functions")
local function fact(n) if n <= 1 then return 1 end return n * fact(n - 1) end
local function fib(n) if n < 2 then return n end return fib(n - 1) + fib(n - 2) end
say(fact(20), fact(25), fib(20))
local function counter() local c = 0; return function() c = c + 1; return c end end
local c1, c2 = counter(), counter(); c1(); c1(); say(c1(), c2())
local function va(...) local a = {...}; return select("#", ...), #a, ... end
say(va(1, nil, 3))
local function deep(n) if n == 0 then return 0 end return 1 + deep(n - 1) end
say(deep(10000))
local function tail(n) if n == 0 then return "tail" end return tail(n - 1) end
say(tail(100000))
say(string.format("%s", (function(...) return select("#", ...) end)(table.unpack({}, 1, 50))))

say("== metatables")
local V = {}
V.__index = V
V.__add = function(a, b) return setmetatable({x = a.x + b.x}, V) end
V.__eq = function(a, b) return a.x == b.x end
V.__lt = function(a, b) return a.x < b.x end
V.__le = function(a, b) return a.x <= b.x end
V.__tostring = function(v) return "V(" .. v.x .. ")" end
V.__len = function(v) return v.x end
V.__call = function(v, y) return v.x * y end
V.__concat = function(a, b) return tostring(a) .. "&" .. tostring(b) end
V.__name = "V"
function V.new(x) return setmetatable({x = x}, V) end
function V:double() return V.new(self.x * 2) end
local a, b = V.new(2), V.new(5)
say(tostring(a + b), a == V.new(2), a < b, b <= a, #b, a(10), a .. b, a:double().x)
local proxy = setmetatable({}, {__index = function(_, k) return k .. "!" end, __newindex = function(t, k, v)
    rawset(t, k, v * 10) end})
proxy.n = 4; say(proxy.hello, proxy.n, rawget(proxy, "zz"))
local weak = setmetatable({}, {__mode = "k"}); weak[{}] = 1; collectgarbage(); say(next(weak))
do
    local log = {}
    do
        local x <close> = setmetatable({}, {__close = function() log[#log + 1] = "closed" end})
        log[#log + 1] = "body"
    end
    say(table.concat(log, ","))
end
local k <const> = 42; say(k)

say("== errors")
say(pcall(error, "msg", 0), pcall(error, {code = 7}), select(2, pcall(error)) == nil)
say(try(function() local x = nil; return x.y end))
say(try(function() return 1 + {} end), try(function() return #5 end), try(function() return ("x") < 1 end))
say(xpcall(function() error("deep", 0) end, function(m) return "handled: " .. m end))
say(select(2, pcall(error, setmetatable({}, {__tostring = function() return "custom" end}))))
local ok, e = pcall(string.rep, "x", -1); say(ok, e == "")
say(try(string.format, "%d", 1.5), try(setmetatable, 1, {}), try(load("return 1 +")))
say(load("syntax error here"))
say(try(function() error("level2", 2) end))
say(tostring(select(2, pcall(function() local t = setmetatable({}, {__index = function(t, k) return t[k] end})
    return t.x end))):match("stack overflow") ~= nil)

say("== coroutines")
local co = coroutine.create(function(x, y)
    local z = coroutine.yield(x + y)
    local w = coroutine.yield(z * 2)
    return w, "done"
end)
say(coroutine.resume(co, 1, 2)); say(coroutine.resume(co, 10)); say(coroutine.resume(co, "w"))
say(coroutine.resume(co), coroutine.status(co))
local gen = coroutine.wrap(function() for i = 1, 3 do coroutine.yield(i) end end)
say(gen(), gen(), gen(), coroutine.isyieldable(), coroutine.running() ~= nil)
local cl = coroutine.create(function() coroutine.yield() end); coroutine.resume(cl)
say(coroutine.close(cl), coroutine.status(cl))
local inner = coroutine.wrap(function() local w = coroutine.wrap(function() coroutine.yield("in") end)
    coroutine.yield(w() .. "-out") end)
say(inner())

say("== goto and loops")
do
    local i, out = 1, {}
    ::top::
    if i <= 3 then out[#out + 1] = i; i = i + 1; goto top end
    say(table.concat(out, ","))
end
for i = 10, 1, -3 do io.write(i, " ") end print()
for i = 1, 3 do for j = 1, 3 do if j == 2 then goto continue end io.write(i, j, " ") ::continue:: end end print()
local n = 0; repeat n = n + 1 until n >= 5; say(n)
for x = 1.0, 2.0, 0.25 do io.write(x, " ") end print()
for i = math.maxinteger - 2, math.maxinteger do io.write(i - math.maxinteger, " ") end print()

say("== load and dump")
local f = load("local a, b = ...; return a * b, _ENV ~= nil")
say(f(6, 7))
local env = {y = 5}
say(load("return y", "chunk", "t", env)(), load("x = 1", "=named", "t", env)(), env.x)
local dumped = string.dump(function(p) return p .. "!" end)
say(load(dumped, "dumped", "b")("bin"), (load(dumped, "d", "t")))
local parts = {"return ", "1 ", "+ ", "41"}
local i = 0
say(load(function() i = i + 1; return parts[i] end)())
say(require("luamod").hello("zenix"), package.loaded.luamod ~= nil, select(2, require("luamod")))
say(pcall(require, "no_such_module_here") == false)

say("== io and os")
local path = tmp .. "/lua-zenix-test.txt"
local fh = assert(io.open(path, "w"))
fh:write("line one\n", 42, "\n", 3.5, "\nlast line without newline")
fh:close()
fh = assert(io.open(path, "r"))
say(fh:read("l"), fh:read("n"), fh:read("n"), fh:read("L"), fh:read("a"), fh:read("l"))
say(fh:seek("set", 5), fh:read(3), fh:seek("cur"), fh:seek("end"))
fh:close()
local count = 0
for line in io.lines(path) do count = count + #line end
say(count, io.type(fh), io.type(io.stdout), io.type(42))
fh = assert(io.open(path, "a")); fh:write("\nappended"); fh:close()
local lines = {}
for l in io.lines(path, "L") do lines[#lines + 1] = l end
say(#lines, lines[#lines])
fh = assert(io.open(path, "rb")); local all = fh:read("a"); fh:close(); say(#all)
say(io.open(tmp .. "/does/not/exist.txt") == nil, select(3, io.open(tmp .. "/does/not/exist.txt")) ~= nil)
local p2 = tmp .. "/lua-zenix-renamed.txt"
say(os.rename(path, p2), io.open(path) == nil, os.remove(p2), io.open(p2) == nil, os.remove(p2) == nil)
local tn = os.tmpname(); say(type(tn), #tn > 0); os.remove(tn)
local pp = io.popen("echo from the shell; echo second")
say(pp:read("l"), pp:read("l"), pp:read("l"), pp:close())
local pw = io.popen("wc -l > " .. tmp .. "/lua-zenix-wc.txt", "w")
pw:write("a\nb\nc\n"); say(pw:close())
local wf = assert(io.open(tmp .. "/lua-zenix-wc.txt")); say((wf:read("l"):gsub("%s", ""))); wf:close()
os.remove(tmp .. "/lua-zenix-wc.txt")
say(os.execute("true"), os.execute("exit 3"), (os.execute()))
say(os.getenv("LUA_ZENIX_TEST_VAR"), type(os.getenv("PATH")))
say(os.time({year = 2026, month = 11, day = 13, hour = 12, min = 0, sec = 0, isdst = false}) -
    os.time({year = 2026, month = 1, day = 1, hour = 12, min = 0, sec = 0, isdst = false}))
say(os.date("!%Y-%m-%d %H:%M:%S %A %B %j", 0), os.date("!%c", 1000000000))
local d = os.date("!*t", 86400 * 365)
say(d.year, d.month, d.day, d.hour, d.wday, d.yday, d.isdst)
say(type(os.time()), type(os.clock()), os.difftime(10, 4), math.type(os.time()))

say("== memory")
local blocks = {}
for j = 1, 200 do blocks[j] = string.rep(string.char(65 + j % 26), 1000) end
say(#blocks, #table.concat(blocks))
blocks = nil
collectgarbage(); collectgarbage()
say(math.type(collectgarbage("count")), collectgarbage("isrunning"), collectgarbage("step", 0) ~= nil)
say(collectgarbage("incremental"), collectgarbage("generational"), collectgarbage("incremental"))
say("== end")
