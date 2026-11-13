#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Memory tests (audit batch B): rootfs/tests/mem.sh under Zenix - malloc/free/realloc and memory calls failing with
# NULL (badptr), a long-running shell keeping its memory flat, directory tools 25 levels deep, and corrupt program
# files (made by tools/mkbadzexe.py, put in /tests by tools/mkroot.sh) refused by the loader. Checks every "ok" line.
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}       # the build directory (make check gives every test a copy of its own)
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
rm -f $BD/zenix.hdf; make -s BUILD=$BD $BD/zenix.hdf >/dev/null 2>&1 || { echo "build failed"; exit 1; }
OUT=${ZENIX_OUT:-$(mktemp -d /tmp/zenix-memtest.XXXXXX)}; mkdir -p $OUT
debugfs -R "stat /tests/badzexe11" $BD/zenix.hdf 2>/dev/null | grep -q 'Mode:  0755' ||
    { echo "FAIL: $BD/zenix.hdf has no /tests/badzexe1-11 (tools/mkroot.sh puts them there)"; exit 1; }
# the program size checks (mem.sh): hello padded to 300 KB, hello cut inside its image, and a 17 MB file (hello's bytes,
# then a hole: the size field set past them), all executable; not in mkroot.sh (the floppies have no room)
python3 - "$BD/hello.zx" "$OUT" "$BD/hello.pzx" <<'PY' || { echo "FAIL: the program size files"; exit 1; }
import sys
d = open(sys.argv[1], "rb").read()
open(sys.argv[2] + "/bigzexe", "wb").write(d + bytes(300 * 1024 - len(d)))
open(sys.argv[2] + "/cutzexe", "wb").write(d[:len(d) // 2])
# more relocations than half the image's bytes (the table inside the file: zeros after it), and the same for hello's
# ZEX2 build (the two lists together against the data image)
import struct
img, ro = struct.unpack(">I", d[8:12])[0], struct.unpack(">I", d[20:24])[0]
n = img // 2 + 1
b = bytearray(d + bytes(max(0, ro + 4 * n - len(d))))
b[16:20] = struct.pack(">I", n)
open(sys.argv[2] + "/relzexe", "wb").write(b)
p = open(sys.argv[3], "rb").read()
ds, nc, ro = struct.unpack(">I", p[12:16])[0], struct.unpack(">I", p[20:24])[0], struct.unpack(">I", p[28:32])[0]
nd = ds // 2 + 1 - nc
b = bytearray(p + bytes(max(0, ro + 4 * (nc + nd) - len(p))))
b[24:28] = struct.pack(">I", nd)
open(sys.argv[2] + "/relzex2", "wb").write(b)
PY
printf '%s\n' "write $OUT/bigzexe /tests/bigzexe" "write $OUT/cutzexe /tests/cutzexe" "write $BD/hello.zx /tests/hugezexe" \
    "sif /tests/hugezexe size 17825792" "sif /tests/bigzexe mode 0100755" "sif /tests/cutzexe mode 0100755" \
    "sif /tests/hugezexe mode 0100755" "write $OUT/relzexe /tests/relzexe" "write $OUT/relzex2 /tests/relzex2" \
    "sif /tests/relzexe mode 0100755" "sif /tests/relzex2 mode 0100755" >$OUT/size.cmd
debugfs -w -f $OUT/size.cmd $BD/zenix.hdf >$OUT/size.log 2>&1
e2fsck -fn $BD/zenix.hdf >$OUT/fsck-before.log 2>&1 || { echo "FAIL: the program size files broke the image ($OUT)"; exit 1; }
cat > $OUT/script.txt <<EOS
wait zenix:/# 90
sleep 1
send sh /tests/mem.sh > /tmp/mem.out 2>&1; sync; echo finished-mem\r
wait (?m)^finished-mem$ ${MEM_WAIT:-330}
stop
EOS
# The time limit, measured 2026-10-26 (audit F7): mem.sh takes about 210 s in FS-UAE (A1200, 68EC020, no FPU), 165 s
# of it awk's 20000 string assignments (the loop's numbers are soft-float doubles: about 8 ms a round); the old
# limit of 200 s timed out every run. 330 s leaves room for a busy machine; the run stops as soon as the script is
# done (after a sync), so the limit costs nothing when all is well.
tools/run-test.sh fsuae/a1200-020-fast.fs-uae $(( ${MEM_WAIT:-330} + 30 )) $OUT/script.txt > $OUT/run.log 2>&1
debugfs -R "cat /tmp/mem.out" $BD/zenix.hdf > $OUT/mem.out 2>/dev/null
cat $OUT/mem.out
ok=$(grep -c ' ok$' $OUT/mem.out); bad=$(grep -c 'FAILED' $OUT/mem.out)
want=$(( 19 + 1 + 8 + 11 + 2 + 4 + 4 ))  # badptr (18 checks + "all ok"), shell churn, directory tools, corrupt programs, kernel log + good program, program sizes, awk
e2fsck -fn $BD/zenix.hdf >$OUT/fsck.log 2>&1 || { echo "FAIL: e2fsck finds errors (see $OUT/fsck.log)"; exit 1; }
if [ "$ok" -eq "$want" ] && [ "$bad" -eq 0 ] && grep -q 'mem: done' $OUT/mem.out; then
    echo "PASS: $ok of $want memory checks ok, e2fsck clean"
else
    echo "FAIL: $ok of $want ok, $bad failed (log: $OUT)"; exit 1
fi
