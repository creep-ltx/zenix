#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# DESIGN D54 (docs/d54-debugger.md section 21): the live debugger in FS-UAE. /tests/dbgtest (built with a frame
# pointer) and its ZEX2 build /tests/dbgtestpure, their maps from the PC's nm -n, and /tests/dbgcheck (the raw calls)
# are put on a copy of the test image; tools/tests/zdb-test.txt drives zdb with the command files in
# rootfs/tests/zdb: a program started under zdb (a breakpoint by name, a variable read twice, the frame chain, a step
# over the breakpoint by the first instruction's length, a variable written, the program's end), a kill, an attach and
# a detach, the shared-code proof (two copies sharing their code run past a breakpoint the third, started under zdb,
# holds in its private copy; an attach to a shared copy refused a breakpoint; the only copy's code disowned and broken
# in), a fault held then ended (139), the crash lines read back by zdb -c, the debugger killed while its program is
# held (an attached one runs on without its breakpoint, a started one is killed), and dbgcheck's checks (permissions,
# nodebug, set-user-id, EFAULT, EAGAIN, a raw step over TRAP #0, a step left pending at a detach, SIGCONT).
# usage: tools/zdb-test.sh [020 | 060 | mmu ...]   (default: 020 060 mmu; mmu: a 68030 with memory protection on)
cd "$(dirname "$0")/.." || exit 1
OUT=${ZENIX_OUT:-/tmp/zenix-zdb}; mkdir -p "$OUT"
BD=${ZENIX_BUILD:-build}
P=${TOOLPREFIX:-$(command -v m68k-elf-gcc >/dev/null && echo m68k-elf- || echo "$HOME/opt/m68k-elf/bin/m68k-elf-")}
make -s BUILD=$BD all "$BD/dbgtest.zx" "$BD/dbgtest.pzx" "$BD/dbgcheck.zx" >"$OUT/build.log" 2>&1 ||
    { echo "FAIL the build"; tail -5 "$OUT/build.log"; exit 1; }
"${P}nm" -n "$BD/dbgtest.uelf" >"$OUT/dbgtest.map" && "${P}nm" -n "$BD/dbgtest.puelf" >"$OUT/dbgtestpure.map" ||
    { echo "FAIL the maps"; exit 1; }
fail=0
for m in ${*:-020 060 mmu}; do
    B=$OUT/build-$m; rm -rf "$B"; cp -a "$BD" "$B" || exit 1
    rm -f "$B/zenix.hdf"; make -s BUILD="$B" "$B/zenix.hdf" >>"$OUT/build.log" 2>&1 || { echo "FAIL the image"; exit 1; }
    set=''
    case $m in
    020) conf=fsuae/a1200-020-fast.fs-uae ;;
    060) conf=fsuae/matrix/a1200-060.fs-uae ;;
    mmu) conf=fsuae/matrix/a1200-030.fs-uae; set='uae_mmu_model=68030'
         python3 tools/kopts.py "$B/kernel.bin" MMU=ON >/dev/null || exit 1
         make -s BUILD="$B" "$B/zenix.adf" >>"$OUT/build.log" 2>&1 || { echo "FAIL the MMU=ON floppy"; exit 1; } ;;
    *) echo "zdb-test: $m: say 020, 060 or mmu"; exit 2 ;;
    esac
    printf '%s\n' "write $B/dbgtest.zx /tests/dbgtest" "write $OUT/dbgtest.map /tests/dbgtest.map" \
        "write $B/dbgtest.pzx /tests/dbgtestpure" "write $OUT/dbgtestpure.map /tests/dbgtestpure.map" \
        "write $B/dbgcheck.zx /tests/dbgcheck" "sif /tests/dbgtest mode 0100755" "sif /tests/dbgtestpure mode 0100755" \
        "sif /tests/dbgcheck mode 0100755" >"$OUT/debugfs-$m.cmd"
    debugfs -w -f "$OUT/debugfs-$m.cmd" "$B/zenix.hdf" >"$OUT/debugfs-$m.log" 2>&1
    e2fsck -fn "$B/zenix.hdf" >"$OUT/e2fsck-before-$m.log" 2>&1 || { echo "FAIL the files on the image ($m)"; exit 1; }
    echo "== $m"
    ZENIX_BUILD="$B" ZENIX_FSUAE_SET="$set${ZENIX_FSUAE_SET:+;$ZENIX_FSUAE_SET}" \
        tools/run-test.sh "$conf" 2400 tools/tests/zdb-test.txt 2>&1 | tr -d '\r' | sed 's/\x1b\[[0-9;:]*[mK]//g' >"$OUT/run-$m.log"
    e2fsck -fn "$B/zenix.hdf" >"$OUT/e2fsck-$m.log" 2>&1; est=$?
    python3 - "$OUT/run-$m.log" "$m" "$est" <<'PY' || fail=1
import re, sys
sys.path.insert(0, "tools")
from screenlog import Log
L = Log(sys.argv[1])
m, est = sys.argv[2], int(sys.argv[3])
check, part = L.check, L.part
def rx(t, pat, what):
    check(re.search(pat, t, re.M) is not None, what, t[-900:])
if m == "mmu":
    check("mmu: memory protection on" in L.text, "memory protection is on", L.text[:1500])
a = part("aout", "aend")
rx(a, r"^started, stopped at its entry 0x[0-9a-f]+ \(_start\)$", "started under zdb: stopped at its entry (_start)")
rx(a, r"^breakpoint at 0x[0-9a-f]+ \(leaf\)$", "b leaf: a breakpoint at the symbol")
rx(a, r"^breakpoint 0x[0-9a-f]+ \(leaf\)\ncounter = 0x0 \(0\)$", "c: held at leaf, the counter read by name: 0")
rx(a, r"^#0 0x[0-9a-f]+ \(leaf\)\n#1 0x[0-9a-f]+ \(middle\+0x[0-9a-f]+\)\n#2 0x[0-9a-f]+ \(outer\+0x[0-9a-f]+\)\n"
      r"#3 0x[0-9a-f]+ \(main\+0x[0-9a-f]+\)$", "bt: leaf, middle, outer, main (the frame chain)")
rx(a, r"^breakpoint 0x[0-9a-f]+ \(leaf\)\ncounter = 0x1 \(1\)$",
   "c again: over the breakpoint, held at its next call, the counter 1")
rx(a, r"^step 0x[0-9a-f]+ \(leaf\+0x2\)$", "s: one instruction (gcc's move.l a6,-(sp): 2 bytes)")
rx(a, r"^counter 15 marker 41$", "set marker 40: the program prints the value written (+1 at i == 3)")
rx(a, r"^exited, status 3$", "the program's end and status")
check(L.status("za") == 0, "zdb's own status", L.status("za"))
b = part("bout", "bend")
rx(b, r"^exited, status 137$", "k: killed (137)")
c = part("cout", "cend")
rx(c, r"^stopped at 0x[0-9a-f]+", "zdb -p: attached and stopped")
rx(c, r"^breakpoint 0x[0-9a-f]+ \(leaf\)\ncounter = 0x[0-9a-f]+ \(([1-9][0-9]*)\)$", "attached: held at leaf, the counter read")
rx(c, r"^detached$", "detach")
rx(c, r"^ +\d+ +\d+ +\d+ +-?\d+ +(?!stop)[a-z]+ +\d+ +\d+ dbgtest$", "after the detach it runs on (ps: not stopped)")
d = part("dout", "dend")
rx(d, r"sharing saves [1-9]\d* KB", "two copies of the ZEX2 program share their code")
rx(d, r"^zdb: its code is shared with other processes: start it under zdb$", "an attach to a shared copy: a breakpoint refused (ETXTBSY)")
rx(d, r"^base 0x[0-9a-f]+ span \d+ code 0x[0-9a-f]+ csize \d+ shared 0$", "started under zdb: its own copy (shared 0)")
rx(d, r"^b1st=3$", "the first sharing copy ran past the breakpoint to its end (status 3)")
rx(d, r"^b2st=3$", "the second too")
check(len(re.findall(r"^counter 15 marker 8$", d, re.M)) == 3, "the two shared copies and the debugged one print their results", d[-900:])
rx(d, r"^breakpoint 0x[0-9a-f]+ \(leaf\)\ncounter = 0x0 \(0\)$", "the debugged copy held at leaf while the others ran")
e = part("eout", "eend")
rx(e, r"^base 0x[0-9a-f]+ span \d+ code 0x[0-9a-f]+ csize \d+ shared 0\nbreakpoint at 0x[0-9a-f]+ \(leaf\)$",
   "attached to the only copy: its code becomes its own, the breakpoint is set")
rx(e, r"^breakpoint 0x[0-9a-f]+ \(leaf\)$", "... and hit")
check(L.status("est") == 3 and L.status("e3") == 3, "it ends normally; the program started again runs (a fresh copy)",
      (L.status("est"), L.status("e3")))
rx(e, r"^damaged=0$", "no shared code was damaged (no DAMAGED line)")
f = part("fout", "fend")
rx(f, r"^fault: vector 5 \(division by zero\), address 0x0\nat 0x[0-9a-f]+ \(boom\+0x[0-9a-f]+\)", "a fault is held first: its vector and place")
rx(f, r"\(boom\+0x[0-9a-f]+\)\n#1 0x[0-9a-f]+ \(b2\+0x[0-9a-f]+\)\n#2 0x[0-9a-f]+ \(b1\+0x[0-9a-f]+\)\n#3 0x[0-9a-f]+ \(main\+0x[0-9a-f]+\)",
   "bt at the fault: boom, b2, b1, main")
rx(f, r"^exited, status 139$", "continued, it ends as without a debugger (139)")
rx(f, r"^crash: pid \d+, vector 5 \(division by zero\), address 0x0\nat 0x[0-9a-f]+ \(boom\+0x[0-9a-f]+\)$",
   "zdb -c: the last crash read from the log, its place by name")
rx(f, r"^#1 0x[0-9a-f]+ \(b2\+0x[0-9a-f]+\)\n#2 0x[0-9a-f]+ \(b1\+0x[0-9a-f]+\)\n#3 0x[0-9a-f]+ \(main\+0x[0-9a-f]+\)",
   "zdb -c: the frames from the kernel's frames line")
rx(f, r"^\[kernel\] pid \d+ regs: base 0x[0-9a-f]+ code 0x[0-9a-f]+ sr [0-9a-f]+ pc 0x[0-9a-f]+ d0\.\.d7( [0-9a-f]+){8} a0\.\.a7( [0-9a-f]+){8}$",
   "the kernel's register line")
rx(f, r"^\[kernel\] pid \d+ frames: [1-8]( 0x[0-9a-f]+)+$", "the kernel's frames line")
g = part("gout", "gend")
rx(g, r"^breakpoint 0x[0-9a-f]+ \(leaf\)$", "attached and held at a breakpoint when zdb was killed")
rx(g, r"^0x[0-9a-f]+: (?!4afc)[0-9a-f]{8}$", "zdb's death took the breakpoint out (leaf's first word is no ILLEGAL)")
rx(g, r"^ +\d+ +\d+ +\d+ +-?\d+ +(?!stop)[a-z]+ +\d+ +\d+ dbgtest$", "the program runs on (ps: not stopped)")
h = part("hout", "hend")
rx(h, r"^breakpoint 0x[0-9a-f]+ \(leaf\)$", "started and held when zdb was killed")
rx(h, r"^left=0$", "the program zdb started (a loop) was killed with it")
k = part("kout", "kend")
for line in re.findall(r"^dbgcheck: (.*) (?:ok|FAILED)$", k, re.M):
    pass
oks = re.findall(r"^dbgcheck: (.*) ok$", k, re.M)
bad = re.findall(r"^dbgcheck: (.*) FAILED$", k, re.M)
check(L.status("zk") == 0 and not bad and len(oks) == 20, "dbgcheck: %d checks ok (permissions, nodebug, set-user-id, "
      "EFAULT, EAGAIN, a raw step over TRAP #0, a pending step at a detach, SIGCONT, a target making a reserved port)" % len(oks), bad or k[-900:])
check("kernel panic" not in L.text and "exception in the kernel" not in L.text, "no kernel panic", L.text[-500:])
check(est == 0, "e2fsck clean after the run", est)
L.waits()
L.done()
PY
done
exit $fail
