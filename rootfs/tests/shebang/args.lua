#!/bin/lua
-- SPDX-License-Identifier: GPL-2.0-only
-- DESIGN D53: what a Lua script started by its #! line sees
print("lua0=" .. arg[0] .. " n=" .. #arg .. " 1=" .. tostring(arg[1]) .. " 2=" .. tostring(arg[2]))
