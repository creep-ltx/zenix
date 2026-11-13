#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Lua in FS-UAE (DESIGN D52 section 9, ports/lua): /bin/lua on the minimum machine (an A1200 with 4 MB of fast RAM).
# rootfs/tests/lua/zenix.lua's output must be the PC's, line for line (tools/lua-host-test.sh builds the same pinned
# source for the PC and runs it: the reference); cstack.lua drives every C recursion a script can make to Lua's own
# limit, and each must end in Lua's "C stack overflow", never in the kernel's stack guard (the stack is fixed, no
# MMU); then the command line: -v, -e, a script from standard input with arguments, a script file with arguments,
# the exit status of an error, a missing file and os.exit, -i (the prompt, a line typed). The memory with a Lua
# running is printed (emulator figures). Lua built by the cross tcc, over 256 KB, runs zenix.lua the same, and a
# copy of it cut short is refused (the loader's program size, 16 MB since 2026-11-13).
cd "$(dirname "$0")/.." || exit 1
OUT=${ZENIX_OUT:-/tmp/zenix-lua}; mkdir -p "$OUT"
BD=${ZENIX_BUILD:-build}
LUA_EXPECTED="$OUT/expected.txt" tools/lua-host-test.sh >"$OUT/host.log" 2>&1 ||
    { echo "FAIL the PC's reference run (tools/lua-host-test.sh)"; cat "$OUT/host.log"; exit 1; }
make -s BUILD=$BD all "$BD/tcc/m68k-tcc" "$BD/devel/usr/lib/libc.a" >"$OUT/build.log" 2>&1 ||
    { echo "FAIL the build"; tail -5 "$OUT/build.log"; exit 1; }
# Lua built by the cross tcc (d52 section 7: about 1.6 times gcc's code, over the 256 KB the loader took until
# 2026-11-13) as /tests/lua/tlua, and a copy cut to 300 KB (its image runs past the file) as /tests/lua/tlua-cut: the
# big one runs zenix.lua like /bin/lua, the cut one is refused
BDA=$(cd "$BD" && pwd); T=$OUT/tlua; rm -rf "$T"; mkdir -p "$T"
cc -std=gnu99 -O2 -o "$T/zld-pc" user/zld.c || { echo "FAIL the PC build of user/zld.c"; exit 1; }
P=${TOOLPREFIX:-$(command -v m68k-elf-gcc >/dev/null && echo m68k-elf- || echo "$HOME/opt/m68k-elf/bin/m68k-elf-")}
objs=()
for f in ports/lua/upstream/src/*.c; do
    n=$(basename "$f" .c); [ "$n" = luac ] && continue
    TMPDIR="$T" "$BDA/tcc/m68k-tcc" -B"$BDA/devel/usr/lib/tcc" -w -DLUA_USE_POSIX -D_setjmp=setjmp -D_longjmp=longjmp \
        -DLUAI_MAXCCALLS=100 -Iports/lua/zenix '-DLUA_USER_H="luauser.h"' -c "$f" -o "$T/$n.o" ||
        { echo "FAIL tcc compiled $f"; exit 1; }
    objs+=("$T/$n.o")
done
# (a 256 KB stack: tcc's frames are bigger than gcc's, and with /bin/lua's 128 KB its 100 C levels of a recursive
# __index overflowed the stack, the guard killing it, 2026-11-13)
"$T/zld-pc" -o "$T/tlua" -s 262144 --defsym=vfprintf=__d_vfprintf \
    --defsym=vfscanf=__d_vfscanf "$BDA/devel/usr/lib/crt0.o" "${objs[@]}" "$BDA/devel/usr/lib/libc.a" \
    "$("${P}gcc" -mcpu=68060 -msoft-float -print-libgcc-file-name)" || { echo "FAIL zld linked tcc's Lua"; exit 1; }
tsz=$(wc -c <"$T/tlua")
[ "$tsz" -gt 262144 ] || { echo "FAIL tcc's Lua is $tsz bytes, not over 256 KB: the test proves nothing"; exit 1; }
isz=$(od -An -tu4 --endian=big -j8 -N4 "$T/tlua" | tr -d ' ')     # its image (header +8): over the cut, so the reason
[ "$isz" -gt 307200 ] || { echo "FAIL tcc's Lua's image is $isz bytes: a cut at 300 KB would not cut into it"; exit 1; }
head -c 307200 "$T/tlua" >"$T/tlua-cut"
rm -f "$BD/zenix.hdf"; make -s BUILD=$BD "$BD/zenix.hdf" >>"$OUT/build.log" 2>&1 || { echo "FAIL the root image"; exit 1; }
printf '%s\n' "write $T/tlua /tests/lua/tlua" "write $T/tlua-cut /tests/lua/tlua-cut" "sif /tests/lua/tlua mode 0100755" \
    "sif /tests/lua/tlua-cut mode 0100755" >"$T/debugfs.cmd"
debugfs -w -f "$T/debugfs.cmd" "$BD/zenix.hdf" >"$T/debugfs.log" 2>&1
e2fsck -fn "$BD/zenix.hdf" >"$T/e2fsck.log" 2>&1 || { echo "FAIL tcc's Lua on the image ($T)"; exit 1; }
echo "     (Lua built by tcc: $tsz bytes; gcc's /bin/lua: $(wc -c <"$BD/lua.zx") bytes)"
ZENIX_BUILD=$BD tools/run-test.sh fsuae/a1200-020-fast.fs-uae 2400 tools/tests/lua-test.txt 2>&1 | tr -d '\r' > "$OUT/run.log"
python3 - "$OUT/run.log" "$OUT/expected.txt" <<'PY'
import re, sys
sys.path.insert(0, "tools")
from screenlog import Log
L = Log(sys.argv[1])
check, part = L.check, L.part
want = open(sys.argv[2], encoding="utf-8").read().splitlines()
got = part("zout", "zend").splitlines()
check(L.status("zst") == 0, "zenix.lua ended with status 0", L.status("zst"))
diff = [(i + 1, w, g) for i, (w, g) in enumerate(zip(want, got)) if w != g]
check(len(got) == len(want) and not diff, "zenix.lua's %d lines are the PC's, line for line" % len(want),
      (len(got), len(want), diff[:5]))
cs = part("cout", "cend")
cases = re.findall(r"^(\w+)\t(.*)\tok$", cs, re.M)
check(L.status("cst") == 0 and len(cases) >= 18 and all(e == "C stack overflow" or (n, e) == ("handler",
      "error in error handling") for n, e in cases) and "cstack done" in cs,
      "every C recursion (%d) stopped by Lua's own limit" % len(cases),
      [c for c in cases if c[1] != "C stack overflow"] or cs[-400:])
# the stack probe build (/tests/lua/luaprobe): every word the stack reached by all the kinds of recursion in one run,
# against /bin/lua's stack (d52 section 9 has the depth of each kind, measured one at a time)
stack = int(re.search(r"^STACK_lua := (\d+)$", open("Makefile").read(), re.M).group(1))
pr = L.part("cend", "pend")
probes = re.findall(r"^luaprobe: stack (\d+) bytes deep(.*)$", pr, re.M)
deepest = int(probes[0][0]) if probes else 0
check(len(probes) == 2 and "cstack done" in pr and not any(x for _, x in probes) and deepest + 8192 <= stack,
      "the deepest stack of all the kinds (%d bytes) leaves 8 KB of STACK_lua (%d) free" % (deepest, stack), pr[-500:])
if len(probes) == 2:
    print("     (the stack's deepest: the C recursions %s bytes, zenix.lua %s bytes)" % (probes[0][0], probes[1][0]))
check("overflowed its user stack" not in L.text, "the stack guard never fired",
      re.findall(r".*overflowed its user stack.*", L.text))
check(re.search(r"^Lua 5\.4\.9  Copyright \(C\) 1994-2026 Lua\.org, PUC-Rio$", L.text, re.M) is not None,
      "lua -v")
check(re.search(r"^2\t2\tlua$", L.text, re.M) is not None, "lua -e: the chunk run, arg as upstream makes it")
check(re.search(r"^fmt=0\.1\|5e-324$", L.text, re.M) is not None, "the C library's %.17g and a subnormal: the "
      "shortest exact digits (picolibc's Ryu; docs/limits.md)", re.findall(r"^fmt=.*", L.text, re.M))
check(re.search(r"^from a pipe\tx\ty$", L.text, re.M) is not None and L.status("pst") == 0,
      "a script from standard input (-) with its arguments")
check(re.search(r"^lua: \(command line\):1: boom$", L.text, re.M) is not None and L.status("est") == 1 and
      "stack traceback:" in L.text, "an error: the message, a traceback, status 1")
check(re.search(r"^lua: cannot open /no/such/file\.lua", L.text, re.M) is not None and L.status("nst") == 1,
      "a missing script: said, status 1")
check(L.status("xst") == 7, "os.exit(7): status 7", L.status("xst"))
check(re.search(r"^args\tone\ttwo$", L.text, re.M) is not None and L.status("ast") == 0,
      "a script file with its arguments")
check(re.search(r"^i=42$", L.text, re.M) is not None, "lua -i: the file run first, then a line typed at the prompt")
m = part("mem", "mend")
ps = re.findall(r"^ +\d+ +\d+ +\d+ +-?\d+ +\w+ +(\d+) +(\d+) lua$", m, re.M)
fr = re.findall(r"^(?:chip RAM|fast RAM|RAM) free: (\d+) KB$", m, re.M)
check(len(ps) == 2, "ps while a Lua runs (one bare, one with 100 KB of strings)", m[-600:])
if len(ps) == 2:
    print("     (memory, A1200 with 4 MB fast RAM, emulator figures: a bare lua owns %s KB (its code, its 128 KB stack, its "
          "heap: a plain program has no shared part), %s KB with 100 KB of strings in its heap; free before and with "
          "it: %s KB)" % (ps[0][0], ps[1][0], " / ".join(fr)))
want_t = part("tout", "tend").splitlines()
check(L.status("tst") == 0 and want_t == want, "Lua built by tcc (over 256 KB) runs zenix.lua: the PC's %d lines" %
      len(want), (L.status("tst"), [(i + 1, w, g) for i, (w, g) in enumerate(zip(want, want_t)) if w != g][:3],
                  len(want_t)))
check(L.status("kst") != 0 and re.search(r"exec: \S*tlua-cut refused: image size outside the file", L.text) is not None,
      "the same Lua cut short is refused (the kernel's reason in its log)", L.status("kst"))
L.waits()
L.done()
PY
