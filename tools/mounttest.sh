#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Mount tests in the emulator: two ext2 partitions mounted on /mnt and /mnt/sub, data across unmount/mount.
# Verified afterwards with the host's e2fsck. Mounts are services (DESIGN D11 6.4): a mount's server runs at nice -10
# with no parent, comes back after a kill, ends for good at umount; a failed mount leaves no service; ordinary ports
# used up and all twelve service slots in use; a floppy without a volume id is not served again after a restart; the
# servers' own messages go to the kernel log (dmesg).
cd "$(dirname "$0")/.." || exit 1
make -s all build/zenix-mount.adf >/dev/null 2>&1 || { echo "build failed"; exit 1; }
OUT=${ZENIX_OUT:-/tmp/zenix-mounttest}; mkdir -p $OUT
pass=1
check() { if [ "$2" = ok ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
dd if=/dev/zero of=build/scratch.hdf bs=1M count=16 status=none
# df1: an ext2 floppy whose superblock carries no uuid (zeros), with one file
head -c 901120 /dev/zero > $OUT/df1.adf
mke2fs -q -t ext2 -E revision=0 -b 1024 -F $OUT/df1.adf >/dev/null 2>&1 || { echo "FAIL mke2fs"; exit 1; }
echo floppy-one > $OUT/f.txt
debugfs -w -R "write $OUT/f.txt f.txt" $OUT/df1.adf >/dev/null 2>&1
dd if=/dev/zero of=$OUT/df1.adf bs=1 seek=1128 count=16 conv=notrunc status=none
rm -rf $OUT/states; mkdir -p $OUT/states
sed "s#^floppy_drive_0 = .*#&\nfloppy_drive_1 = $OUT/df1.adf\nfloppy_drive_count = 2\nwritable_floppy_images = 1\nsave_states_dir = $OUT/states#" fsuae/mounts.fs-uae > $OUT/mounts.fs-uae
ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenixfs-serial} tools/run-test.sh $OUT/mounts.fs-uae 230 tools/tests/mount-test.txt > $OUT/mount.log 2>&1
grep -q 'ide0p1 .*/mnt .*ext2' $OUT/mount.log && check "mount lists the first filesystem" ok || check "mount lists the first filesystem" bad
grep -q 'ide0p2 .*/mnt/sub .*ext2' $OUT/mount.log && check "nested mount works" ok || check "nested mount works" bad
grep -aq '^fdisk: ide0 is in use (a partition on it is mounted)' $OUT/mount.log && check "fdisk does not write a disk with a mounted partition" ok || check "fdisk does not write a disk with a mounted partition" bad
grep -qx 'hello-one' $OUT/mount.log && grep -qx 'hello-two' $OUT/mount.log && check "files read back through both mounts" ok || check "files read back through both mounts" bad
grep -q '^/mnt/sub' $OUT/mount.log && check "pwd inside a mount" ok || check "pwd inside a mount" bad
sed -n '/END-OF-BUSY-TEST/q;p' $OUT/mount.log | grep -q 'is busy' && check "umount refuses a mount with another mount below it" ok || check "umount refuses a mount with another mount below it" bad
sed -n '/END-OF-BUSY-TEST/,$p' $OUT/mount.log | grep -q 'is busy' && check "no other umount was refused" bad || check "no other umount was refused" ok
tr -d '\r' < $OUT/mount.log | awk '/MOUNTED ON/{buf=""} {buf=buf"\n"$0} END{print buf}' | grep -q 'ide0p' && check "everything is unmounted at the end" bad || check "everything is unmounted at the end" ok
L=$OUT/mount.log
has() { if tr -d '\r' < $L | grep -aqE -- "$2"; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
has "a mount's server runs with no parent at nice -10 (two fsd lines)" '^ +[0-9]+ +0 +[0-9]+ +-10 [a-z]+ +[0-9]+ fsd$'
[ "$(tr -d '\r' < $L | grep -acE '^ +[0-9]+ +0 +[0-9]+ +-10 [a-z]+ +[0-9]+ fsd$')" -ge 2 ] && check "... the root's and the mount's" ok || check "... the root's and the mount's" bad
has "the mount's server killed: the log says it ended" '^svc: fs\.ide0p1: fsd \(pid [0-9]+\) ended, status 130$'
has "... and the next use started it again" '^svc: fs\.ide0p1: started @fsd \(pid [0-9]+, for pid [0-9]+ cat\)$'
has "... a file written before the kill reads back" '^(.*# )?kept-across$'
has "umount ends it for good (only the root's fsd left)" '^(.*# )?fsd-left=1$'
has "... and its name is gone" 'svctest call fs\.ide0p1 1: -1 in [0-9]+ ms$'
has "a failed mount says why" '^mount: cannot mount ide0p2: its filesystem server did not start \(see dmesg\)$'
has "... and leaves no service (the name is free again)" 'svctest port fs\.ide0p2: [0-9]+$'
has "ordinary ports used up" 'svctest ports: 16 made, then -2$'
[ "$(tr -d '\r' < $L | grep -acE '^(.*# )?kept-across$')" -ge 3 ] && check "... root's mount still works meanwhile; umount then mount at once with all twelve slots in use works" ok || check "... root's mount still works meanwhile; umount then mount at once with all twelve slots in use works" bad
has "a thirteenth service is refused" 'svctest reg d10: -16$'
has "... and mount says so" '^mount: too many filesystems mounted$'
has "a floppy reads through its mount" '^(.*# )?floppy-one$'
has "a floppy without a volume id is not served again after a restart" '^fs\.df1: a floppy without a volume id is not served again after a restart: mount it again$'
has "the servers' messages are in the kernel log: diskd's (a dead server's claim released at the restart)" '^(.*# )?inlog-diskd=[1-9]$'
has "... zfsd's (not a ZenixFS volume)" '^(.*# )?inlog-zfsd=[1-9]$'
has "... fsd's (the floppy refused at its restart)" '^(.*# )?inlog-fsd=[1-9]$'
has "the script ran to the end" '^(.*# )?mount-svc-done$'
# umount while a program is being loaded from the mount (audit A3): refused, the load completes; a floppy's reads are
# slow enough to umount in the middle
head -c 901120 /dev/zero > $OUT/df1l.adf
mke2fs -q -t ext2 -E revision=0 -b 1024 -F $OUT/df1l.adf >/dev/null 2>&1 || { echo "FAIL mke2fs"; exit 1; }
debugfs -w -R "write build/awk.zx awk" $OUT/df1l.adf >/dev/null 2>&1
debugfs -w -R "sif awk mode 0100755" $OUT/df1l.adf >/dev/null 2>&1
cp build/zenix.hdf $OUT/lroot.hdf
sed "s#^floppy_drive_0 = .*#&\nfloppy_drive_1 = $OUT/df1l.adf\nfloppy_drive_count = 2#; s#^floppy_drive_speed = .*#floppy_drive_speed = 100#; s#^hard_drive_0 = .*#hard_drive_0 = $OUT/lroot.hdf#" fsuae/a1200-020-fast.fs-uae > $OUT/load.fs-uae
ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenixfs-serial} tools/run-test.sh $OUT/load.fs-uae 150 tools/tests/mount-load-test.txt > $OUT/load.log 2>&1
L=$OUT/load.log
has "umount during a program load from the mount is refused" '^(.*# )?um=1$'
has "... the program loaded and ran" '^(.*# )?loaded-ok$'
has "... and the umount after it works" '^(.*# )?um2=0$'
rm -f $OUT/df1l.adf $OUT/lroot.hdf
# the unmounted partitions are clean ext2 as seen by the host (partitions start at cylinder 1 and 14 of 1008 sectors)
for spec in "1 13104" "14 18144"; do set -- $spec
  dd if=build/scratch.hdf of=$OUT/p$1.img bs=512 skip=$(( $1 * 1008 )) count=$2 status=none
  e2fsck -fn $OUT/p$1.img >$OUT/e2fsck$1.log 2>&1 && check "host e2fsck: partition at cylinder $1 is clean" ok || check "host e2fsck: partition at cylinder $1 is clean" bad
done
exit $((1 - pass))
