#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# zas in FS-UAE (DESIGN D52 section 6, docs/man/zas.txt): /bin/zas on the minimum machine (an A1200 with 4 MB of fast
# RAM) assembles a small program (tools/tests/zas/prog.s: macros from an included file, dbra loops, mulu.l, divul.l,
# a bit field, a PC-relative table built with .irp, a string built in the bss), zld links it with crt0 and libz, and it
# runs; then zas assembles gcc's output for user/sort.c (with its debugging data) while ps watches its memory. Checked:
# each object Zenix made is the PC's zas object byte for byte (which tools/zas-host-test.sh proves against GNU as), the
# program prints its five lines and ends with status 3, and a source with an error is refused with status 1, its
# message and no object. Emulator figures, not hardware speed. Needs debugfs and e2fsck (e2fsprogs).
cd "$(dirname "$0")/.." || exit 1
OUT=${ZENIX_OUT:-/tmp/zenix-zas}; mkdir -p "$OUT"
BD=${ZENIX_BUILD:-build}
P=${TOOLPREFIX:-$(command -v m68k-elf-gcc >/dev/null && echo m68k-elf- || echo "$HOME/opt/m68k-elf/bin/m68k-elf-")}
make -s BUILD=$BD all >"$OUT/build.log" 2>&1 || { echo "FAIL the build"; tail -5 "$OUT/build.log"; exit 1; }
F=$OUT/files; rm -rf "$F"; mkdir -p "$F"
cc -std=gnu99 -O2 -o "$OUT/zas-pc" user/zas.c && cc -std=gnu99 -O2 -o "$OUT/zld-pc" user/zld.c ||
    { echo "FAIL the PC builds of zas and zld"; exit 1; }
cp tools/tests/zas/prog.s tools/tests/zas/progmac.i "$BD/devel/usr/lib/crt0.o" "$BD/devel/usr/lib/libgcc.a" "$F/"
"${P}objcopy" --strip-debug "$BD/libz.uo" "$F/libz.o" || { echo "FAIL objcopy libz"; exit 1; }
"${P}gcc" -m68020-60 -msoft-float -ffreestanding -fno-builtin -nostdlib -O2 -g -Wall -fno-common -ffunction-sections \
    -fdata-sections -S user/sort.c -o "$F/sort.s" || { echo "FAIL gcc -S sort.c"; exit 1; }
printf '\tnop\n\tmove.l %%d0,#5\n\tfrob\n' >"$F/bad.s"
# the PC's objects and program, from the same files
(cd "$F" && "$OUT/zas-pc" -m68020 -o "$OUT/pc-prog.o" prog.s && "$OUT/zas-pc" -m68040 -mno-float -o "$OUT/pc-sort.o" sort.s &&
    "$OUT/zld-pc" -o "$OUT/pc-prog" crt0.o "$OUT/pc-prog.o" libz.o libgcc.a) || { echo "FAIL the PC's runs"; exit 1; }
H=$BD/zenix.hdf
rm -f "$H"; make -s BUILD=$BD "$H" >>"$OUT/build.log" 2>&1 || { echo "FAIL the root image"; exit 1; }   # a fresh one
dfs() { debugfs -w -R "$1" "$H" >/dev/null 2>&1; }
dfs "mkdir /root/zas"
for f in "$F"/*; do
    dfs "write $f /root/zas/$(basename "$f")"
done
e2fsck -fn "$H" >"$OUT/e2fsck-before.log" 2>&1 || { echo "FAIL the image after writing the files"; exit 1; }
ZENIX_BUILD=$BD tools/run-test.sh fsuae/a1200-020-fast.fs-uae 3600 tools/tests/zas-test.txt 2>&1 | tr -d '\r' >"$OUT/run.log"
for n in prog.o prog sort.o bad.o; do
    rm -f "$OUT/z-$n"
    debugfs -R "dump /root/zas/$n $OUT/z-$n" "$H" >/dev/null 2>&1
done
e2fsck -fn "$H" >"$OUT/e2fsck.log" 2>&1; est=$?
python3 - "$OUT" "$est" <<'PY'
import os, re, sys
sys.path.insert(0, "tools")
from screenlog import Log
out, est = sys.argv[1], int(sys.argv[2])
L = Log(os.path.join(out, "run.log"))
check = L.check
t = L.text
def same(z, pc):
    z, pc = os.path.join(out, z), os.path.join(out, pc)
    return os.path.exists(z) and open(z, "rb").read() == open(pc, "rb").read()
check(L.status("pst") == 0, "zas assembled prog.s on Zenix (an included file, macros), status 0", L.status("pst"))
check(same("z-prog.o", "pc-prog.o"), "its object is the PC's byte for byte")
check(L.status("lst") == 0 and same("z-prog", "pc-prog"), "zld linked it, the PC's program byte for byte", L.status("lst"))
want = ["sum 5050", "product 83810205", "quotient 142857 remainder 1", "field 67", "squares 385", "hello from zas!"]
got = all(re.search("^" + re.escape(w) + "$", t, re.M) for w in want)
check(L.status("run") == 3 and got, "the program runs: its lines and status 3", (L.status("run"), [w for w in want if w not in t]))
check(L.status("sst") == 0 and same("z-sort.o", "pc-sort.o"),
      "zas assembled gcc's output for sort.c (%d bytes of source): the PC's object byte for byte"
      % os.path.getsize(os.path.join(out, "files", "sort.s")), L.status("sst"))
check(L.status("bst") == 1 and re.search(r"^zas: bad\.s:2: operands not valid for this instruction$", t, re.M) is not None and
      re.search(r"^zas: bad\.s:3: unknown instruction frob$", t, re.M) is not None and not os.path.exists(os.path.join(out, "z-bad.o")),
      "a source with errors: status 1, each error with its line, no object", L.status("bst"))
check(est == 0, "the host's e2fsck finds the root clean", est)
check("overflowed its user stack" not in t, "the stack guard never fired", re.findall(r".*overflowed.*", t))
ps = [int(a) for a in re.findall(r"^ +\d+ +\d+ +\d+ +-?\d+ +\w+ +(\d+) +\d+ zas$", t, re.M)]
fr = re.findall(r"^(?:chip RAM|fast RAM|RAM) free: (\d+) KB$", t, re.M)
times = re.findall(r"^real\s+(\S+)$", t, re.M)
check(len(ps) > 0, "ps saw zas while it assembled sort.s", t[-400:])
if ps:
    print("     (emulator figures, A1200 with 4 MB fast RAM: zas on sort.s owned at most %d KB in %d samples; free "
          "before: %s KB; prog.s took %s)" % (max(ps), len(ps), " / ".join(fr[:2]), times[0] if times else "?"))
L.waits()
L.done()
PY
st=$?
# the copy of the build's image is the test's: put it back as the build makes it
rm -f "$H"; make -s BUILD=$BD "$H" >/dev/null 2>&1
exit $st
