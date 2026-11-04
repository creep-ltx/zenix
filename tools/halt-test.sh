#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# halt, poweroff and shutdown (reboot(8)): a wrong argument is refused (status 2); "shutdown now" saves the disks as
# reboot does, stops disk writes and halts with the line saying the Amiga may be switched off; the host's e2fsck then
# finds the root clean and the file written just before is on it.
cd "$(dirname "$0")/.." || exit 1
OUT=${ZENIX_OUT:-/tmp/zenix-halt}; mkdir -p $OUT
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >$OUT/build.log 2>&1 || { echo "FAIL the build"; tail -5 $OUT/build.log; exit 1; }
rm -f $BD/zenix.hdf
make -s BUILD=$BD $BD/zenix.hdf >>$OUT/build.log 2>&1 || { echo "FAIL the image"; tail -5 $OUT/build.log; exit 1; }
cp $BD/zenix.hdf $OUT/root.hdf
sed "s#../build/zenix.hdf#$OUT/root.hdf#" fsuae/a1200-020-fast.fs-uae > $OUT/h.fs-uae
ZENIX_BUILD=$BD tools/run-test.sh $OUT/h.fs-uae 300 tools/tests/halt-test.txt 2>&1 | tr -d '\r' > $OUT/run.log
pass=1
ok() { echo "PASS $1"; }
bad() { echo "FAIL $1"; pass=0; }
grep -q '^FAIL' $OUT/run.log && bad "the script: $(grep '^FAIL' $OUT/run.log | head -1)"
grep -q 'usage: halt' $OUT/run.log && ok "halt with an argument: its usage, status 2" || bad "halt with an argument"
grep -q 'halt: filesystems synced, disk writes stopped' $OUT/run.log && ok "shutdown now: filesystems synced, disk writes stopped" || bad "no sync line"
grep -q 'The system is halted. You can switch the Amiga off now' $OUT/run.log && ok "... and the halted line" || bad "no halted line"
e2fsck -fn $OUT/root.hdf > $OUT/e2fsck.log 2>&1 && ok "the host's e2fsck finds the root clean" || bad "e2fsck: $(tail -2 $OUT/e2fsck.log)"
[ "$(debugfs -R 'cat /halt.txt' $OUT/root.hdf 2>/dev/null)" = written-before-halt ] && ok "the file written just before is on the disk" || bad "the file is not on the disk"
[ $pass = 1 ] && echo "halt-test: all passed" || { echo "halt-test: FAILED (log $OUT/run.log)"; exit 1; }
