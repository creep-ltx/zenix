#!/usr/bin/env bash
# Regression test for fdisk, mkfs.ext2 and fsck.ext2 in the emulator, verified with the host's e2fsck.
cd "$(dirname "$0")/.." || exit 1
make -s all build/zenix-ram.adf >/dev/null 2>&1 || { echo "build failed"; exit 1; }
OUT=/tmp/zenix-disktools; mkdir -p $OUT
pass=1
check() { if [ "$2" = ok ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }

# 1. fdisk + mkfs.ext2 on a blank disk
dd if=/dev/zero of=build/scratch.hdf bs=1M count=16 status=none
tools/run-test.sh fsuae/tools/disktools.fs-uae 80 tools/disktools-test.txt > $OUT/mkfs.log 2>&1
grep -q 'ide0p2   data' $OUT/mkfs.log && check "fdisk creates partitions" ok || check "fdisk creates partitions" bad
grep -q 'cannot open ram0' $OUT/mkfs.log && check "mkfs refuses a mounted device" ok || check "mkfs refuses a mounted device" bad
for spec in "1 13104" "14 18144"; do
  set -- $spec
  dd if=build/scratch.hdf of=$OUT/p$1.img bs=512 skip=$(( $1 * 1008 )) count=$2 status=none
  e2fsck -fn $OUT/p$1.img >$OUT/e2fsck$1.log 2>&1 && check "mkfs.ext2 result clean (partition at cylinder $1)" ok || check "mkfs.ext2 result clean (partition at cylinder $1)" bad
done

# 2. fsck.ext2 on a damaged filesystem
tools/mkbadfs.sh
tools/run-test.sh fsuae/tools/disktools.fs-uae 90 tools/fsck-test.txt > $OUT/fsck.log 2>&1
grep -q 'problem(s) found' $OUT/fsck.log && check "fsck finds the damage" ok || check "fsck finds the damage" bad
grep -q 'fsck.ext2: clean' $OUT/fsck.log && check "fsck repairs it (clean on the last run)" ok || check "fsck repairs it" bad
dd if=build/scratch.hdf of=$OUT/fixed.img bs=512 skip=1008 count=8192 status=none
e2fsck -fn $OUT/fixed.img >$OUT/e2fsck-fixed.log 2>&1 && check "host e2fsck agrees the repaired disk is clean" ok || check "host e2fsck agrees" bad
exit $((1 - pass))
