#!/usr/bin/env bash
# Mount tests in the emulator: two ext2 partitions mounted on /mnt and /mnt/sub, data across unmount/mount.
# Verified afterwards with the host's e2fsck.
cd "$(dirname "$0")/.." || exit 1
make -s all build/zenix-ram.adf >/dev/null 2>&1 || { echo "build failed"; exit 1; }
OUT=/tmp/zenix-mounttest; mkdir -p $OUT
pass=1
check() { if [ "$2" = ok ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
dd if=/dev/zero of=build/scratch.hdf bs=1M count=16 status=none
ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenixfs-serial} tools/run-test.sh fsuae/mounts.fs-uae 150 tools/tests/mount-test.txt > $OUT/mount.log 2>&1
grep -q 'ide0p1 .*/mnt .*ext2' $OUT/mount.log && check "mount lists the first filesystem" ok || check "mount lists the first filesystem" bad
grep -q 'ide0p2 .*/mnt/sub .*ext2' $OUT/mount.log && check "nested mount works" ok || check "nested mount works" bad
grep -q 'hello-one' $OUT/mount.log && grep -q 'hello-two' $OUT/mount.log && check "files read back through both mounts" ok || check "files read back through both mounts" bad
grep -q '^/mnt/sub' $OUT/mount.log && check "pwd inside a mount" ok || check "pwd inside a mount" bad
sed -n '/END-OF-BUSY-TEST/q;p' $OUT/mount.log | grep -q 'is busy' && check "umount refuses a mount with another mount below it" ok || check "umount refuses a mount with another mount below it" bad
sed -n '/END-OF-BUSY-TEST/,$p' $OUT/mount.log | grep -q 'is busy' && check "no other umount was refused" bad || check "no other umount was refused" ok
tr -d '\r' < $OUT/mount.log | awk '/MOUNTED ON/{buf=""} {buf=buf"\n"$0} END{print buf}' | grep -q 'ide0p' && check "everything is unmounted at the end" bad || check "everything is unmounted at the end" ok
# the unmounted partitions are clean ext2 as seen by the host (partitions start at cylinder 1 and 14 of 1008 sectors)
for spec in "1 13104" "14 18144"; do set -- $spec
  dd if=build/scratch.hdf of=$OUT/p$1.img bs=512 skip=$(( $1 * 1008 )) count=$2 status=none
  e2fsck -fn $OUT/p$1.img >$OUT/e2fsck$1.log 2>&1 && check "host e2fsck: partition at cylinder $1 is clean" ok || check "host e2fsck: partition at cylinder $1 is clean" bad
done
exit $((1 - pass))
