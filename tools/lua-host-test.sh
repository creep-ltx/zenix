#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Lua on the PC (DESIGN D52 section 9, ports/lua): the pinned Lua 5.4.9 source built for the PC with the settings
# Zenix's build gives it (the Makefile's LUA_CFLAGS: LUA_USE_POSIX, LUAI_MAXCCALLS, ports/lua/zenix/luauser.h) under
# AddressSanitizer and UBSan, then rootfs/tests/lua/zenix.lua and cstack.lua run with it. Checked: both end with
# status 0 and print their last line, and every C-stack case ends in Lua's own "C stack overflow" (the message
# handler's in "error in error handling"). zenix.lua's os.tmpname makes and removes one file in /tmp (upstream's
# fixed template). The output of zenix.lua is the reference tools/lua-test.sh compares Zenix's output with, line for
# line: LUA_EXPECTED=FILE keeps a copy of it.
cd "$(dirname "$0")/.." || exit 1
T=$(mktemp -d "${TMPDIR:-/tmp}/zenix-lua.XXXXXX")
trap 'rm -rf "$T"' EXIT
fail=0
check() { if [ "$1" = 0 ]; then echo "PASS $2"; else echo "FAIL $2${3:+: $3}"; fail=1; fi; }
maxc=$(sed -n 's/^LUA_MAXCCALLS := //p' Makefile)
[ -n "$maxc" ] || { echo "FAIL LUA_MAXCCALLS not found in the Makefile"; exit 1; }
cc -std=gnu99 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -DLUA_USE_POSIX -DLUAI_MAXCCALLS="$maxc" \
    -Iports/lua/zenix '-DLUA_USER_H="luauser.h"' -o "$T/lua" ports/lua/upstream/src/*.c -lm 2>"$T/cc.log"
check $? "the PC build of ports/lua/upstream (LUAI_MAXCCALLS $maxc)" "$(head -3 "$T/cc.log")"
[ $fail = 0 ] || exit 1
mkdir -p "$T/scratch"
# ASan's own stack is far bigger than Zenix's; the C-stack cases check Lua's limit, tools/lua-test.sh the stack
(cd rootfs/tests/lua && env -u LUA_ZENIX_TEST_VAR ASAN_OPTIONS=detect_leaks=1 "$T/lua" zenix.lua "$T/scratch") \
    >"$T/out.txt" 2>"$T/err.txt"
st=$?
check $st "zenix.lua ran to its end with status 0" "$(tail -3 "$T/err.txt")"
[ "$(tail -1 "$T/out.txt")" = "== end" ]; check $? "zenix.lua printed its last line" "$(tail -1 "$T/out.txt")"
n=$(grep -c '' "$T/out.txt"); [ "$n" -gt 100 ]; check $? "zenix.lua printed $n lines (more than 100)"
[ -z "$(ls -A "$T/scratch")" ]; check $? "zenix.lua left no scratch files" "$(ls "$T/scratch")"
grep -q "^9223372036854775807	-9223372036854775808	true$" "$T/out.txt"; check $? "64-bit integers"
grep -qx "== version	Lua 5.4	integer	float	nil" "$T/out.txt"; check $? "the version line"
(cd rootfs/tests/lua && "$T/lua" cstack.lua) >"$T/cs.txt" 2>&1
check $? "cstack.lua ran with status 0" "$(tail -3 "$T/cs.txt")"
ncase=$(grep -cE "	(C stack overflow|error in error handling)	ok$" "$T/cs.txt"); nall=$(grep -c "	ok$" "$T/cs.txt")
[ "$ncase" = "$nall" ] && [ "$nall" -ge 18 ] && grep -qx "handler	error in error handling	ok" "$T/cs.txt" &&
    grep -qx "cstack done" "$T/cs.txt"
check $? "every C-stack case stopped by Lua's own limit ($ncase of $nall)" "$(grep -vE 'C stack overflow|error in error' "$T/cs.txt")"
[ -n "$LUA_EXPECTED" ] && cp "$T/out.txt" "$LUA_EXPECTED"
exit $fail
