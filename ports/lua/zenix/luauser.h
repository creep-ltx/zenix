/* SPDX-License-Identifier: GPL-2.0-only */
/* Zenix's settings for Lua (DESIGN D52 section 9), through upstream's own hook: lua.h includes the file LUA_USER_H
 * names after luaconf.h, so what is set here holds for every file that includes lua.h, and upstream's files stay
 * unchanged. The C stack is fixed (no MMU), so the frames that recur once a C level are kept small:
 * LUAL_BUFFERSIZE is the buffer every luaL_Buffer carries on the stack (string.gsub, string.format, table.concat,
 * the traceback ...): upstream's 16 * sizeof(void *) * sizeof(lua_Number) is 512 bytes on the 68k; 256 here, a
 * longer result moves to the heap as upstream's code does for any buffer that outgrows it. */
#undef LUAL_BUFFERSIZE
#define LUAL_BUFFERSIZE 256
