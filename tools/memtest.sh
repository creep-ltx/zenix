#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Memory tests (audit batch B): rootfs/tests/mem.sh under Zenix - malloc/free/realloc and memory calls failing with
# NULL (badptr), a long-running shell keeping its memory flat, directory tools 25 levels deep, and corrupt program
# files (made by tools/mkbadzexe.py, put in /tests by tools/mkroot.sh) refused by the loader. Checks every "ok" line.
cd "$(dirname "$0")/.." || exit 1
make -s all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
rm -f build/zenix.hdf; make -s build/zenix.hdf >/dev/null 2>&1 || { echo "build failed"; exit 1; }
OUT=$(mktemp -d /tmp/zenix-memtest.XXXXXX)
debugfs -R "stat /tests/badzexe11" build/zenix.hdf 2>/dev/null | grep -q 'Mode:  0755' ||
    { echo "FAIL: build/zenix.hdf has no /tests/badzexe1-11 (tools/mkroot.sh puts them there)"; exit 1; }
cat > $OUT/script.txt <<EOS
wait zenix:/# 90
sleep 1
send sh /tests/mem.sh > /tmp/mem.out 2>&1; echo finished-mem\r
wait (?m)^finished-mem$ ${MEM_WAIT:-200}
EOS
tools/run-test.sh fsuae/a1200-020-fast.fs-uae $(( ${MEM_WAIT:-200} + 30 )) $OUT/script.txt > $OUT/run.log 2>&1
debugfs -R "cat /tmp/mem.out" build/zenix.hdf > $OUT/mem.out 2>/dev/null
cat $OUT/mem.out
ok=$(grep -c ' ok$' $OUT/mem.out); bad=$(grep -c 'FAILED' $OUT/mem.out)
want=$(( 13 + 1 + 8 + 11 + 2 ))      # badptr (12 checks + "all ok"), shell churn, directory tools, corrupt programs, kernel log + good program
e2fsck -fn build/zenix.hdf >$OUT/fsck.log 2>&1 || { echo "FAIL: e2fsck finds errors (see $OUT/fsck.log)"; exit 1; }
if [ "$ok" -eq "$want" ] && [ "$bad" -eq 0 ] && grep -q 'mem: done' $OUT/mem.out; then
    echo "PASS: $ok of $want memory checks ok, e2fsck clean"
else
    echo "FAIL: $ok of $want ok, $bad failed (log: $OUT)"; exit 1
fi
