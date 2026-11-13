-- SPDX-License-Identifier: GPL-2.0-only
-- A module for rootfs/tests/lua/zenix.lua's require test (DESIGN D52 section 9).
local M = {}
function M.hello(who) return "hello, " .. who end
return M
