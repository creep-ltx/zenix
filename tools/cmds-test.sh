#!/usr/bin/env bash
# Run rootfs/tests/cmds.sh under Zenix (output to a file on its disk) and under the host's bash with GNU tools
# (LC_ALL=C); the two outputs must be identical.
cd "$(dirname "$0")/.." || exit 1
make -s all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
rm -f build/zenix.hdf; make -s build/zenix.hdf >/dev/null 2>&1
OUT=/tmp/zenix-cmds; mkdir -p $OUT
( cd rootfs/home/t && LC_ALL=C bash ../../tests/cmds.sh "$PWD" > $OUT/expected.txt 2>&1 )
cat > $OUT/script.txt <<EOS
sleep 22
send sh /tests/cmds.sh > /tmp/cmds.out 2>&1; echo finished-cmds\r
sleep ${CMDS_WAIT:-100}
EOS
tools/run-test.sh fsuae/a1200-020-fast.fs-uae $(( ${CMDS_WAIT:-100} + 30 )) $OUT/script.txt > $OUT/run.log 2>&1
debugfs -R "cat /tmp/cmds.out" build/zenix.hdf > $OUT/actual.txt 2>/dev/null
if diff $OUT/expected.txt $OUT/actual.txt > $OUT/diff.txt; then echo "PASS: all command outputs identical to GNU ($(wc -l < $OUT/expected.txt) lines)"; else echo "FAIL: differences:"; head -60 $OUT/diff.txt; exit 1; fi
# the disk after the run (C1: rm of fast symbolic links used to free their text as block numbers)
if e2fsck -fn build/zenix.hdf > $OUT/e2fsck.log 2>&1; then echo "PASS: host e2fsck -fn clean after the run"; else echo "FAIL: e2fsck finds errors (see $OUT/e2fsck.log)"; exit 1; fi
