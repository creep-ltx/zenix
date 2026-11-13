/* SPDX-License-Identifier: GPL-2.0-only */
/* Zenix's settings for Lua (DESIGN D52 section 9), through upstream's own hook: lua.h includes the file LUA_USER_H
 * names after luaconf.h, so what is set here holds for every file that includes lua.h, and upstream's files stay
 * unchanged. The C stack is fixed (no MMU), so what recurs once per C level is kept small and bounded:
 * - LUAL_BUFFERSIZE is the buffer every luaL_Buffer carries on the stack (string.gsub, string.format, table.concat,
 *   the traceback ...): upstream's 16 * sizeof(void *) * sizeof(lua_Number) is 512 bytes on the 68k; 256 here, a
 *   longer result moves to the heap as upstream's code does for any buffer that outgrows it.
 * - MAXCCALLS is the pattern matcher's own recursion limit (lstrlib.c, a counter of its own beside LUAI_MAXCCALLS):
 *   upstream's 200 levels of match() on top of the deepest C chain would cost about 14 KB more; 100 here ("pattern
 *   too complex" past it). */
#undef LUAL_BUFFERSIZE
#define LUAL_BUFFERSIZE 256
#define MAXCCALLS 100

/* The test build's stack probe (/tests/lua/luaprobe, tools/lua-test.sh): the stack below the state's start painted
 * when the state opens and the deepest word written found when it closes, said on standard error. Not in /bin/lua. */
#ifdef ZENIX_LUA_STACKPROBE
void zx_stackprobe_open(void);
void zx_stackprobe_close(void);
#define luai_userstateopen(L) ((void)(L), zx_stackprobe_open())
#define luai_userstateclose(L) ((void)(L), zx_stackprobe_close())
#endif
