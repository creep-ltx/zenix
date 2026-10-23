#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# ZenixFS in the emulator: partition, mkfs.zenixfs, mount next to an ext2 mount, files / links / rename / a 100 KB file,
# unmount, remount, fsck.zenixfs; then the HOST checks the same partition with zenixfs-tool (independent implementation
# of the check: the same library source, but a different program, block device and compiler).
cd "$(dirname "$0")/.." || exit 1
make -s all build/zenix-mount.adf >/dev/null 2>&1 || { echo "build failed"; exit 1; }
make -s -C zenixfs build/zenixfs-tool >/dev/null 2>&1 || { echo "host tool build failed"; exit 1; }
OUT=/tmp/zenix-zenixfstest; mkdir -p $OUT
pass=1
check() { if [ "$2" = ok ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
dd if=/dev/zero of=build/scratch.hdf bs=1M count=16 status=none
ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenixfs-serial} tools/run-test.sh fsuae/mounts.fs-uae 330 tools/tests/zenixfs-test.txt 2>&1 | tr -d '\r' | sed 's/\x1b\[K//g' > $OUT/run.log
# the log also holds the keystrokes the test typed, so a check must match a whole OUTPUT line, never a substring of the typing
wantline() { if grep -aqx -- "$2" $OUT/run.log; then check "$1" ok; else check "$1" bad; fi; }
wantre() { if grep -aqE -- "$2" $OUT/run.log; then check "$1" ok; else check "$1" bad; fi; }
wantre "fdisk shows a zenixfs partition" '^ide0p2 .* zenixfs$'
wantre "mkfs.zenixfs runs" '^mkfs.zenixfs: ide0p2: [0-9]+ blocks of [0-9]+ bytes$'
wantre "mount lists the zenixfs volume (type read from the partition table)" '^ide0p2 +/mnt +zenixfs '
wantline "file read back through a symbolic link" "hello-zenixfs"
wantre "ls -l shows the symbolic link" '^lrwx.* link -> a.txt$'
wantre "hard link count 2 for the file" '^-rw-r--r-- 2 root root +14 .* a.txt$'
wantre "wc sees the 100 KB file: 20000 lines, 108894 bytes" '^ *20000 +20000 +108894 /mnt/seq.txt$'
wantline "copy of the large file is identical" "SAME-SEQ"
wantline "copy to an ext2 mount is identical" "SAME-ACROSS-MOUNTS"
wantre "mv across mounts leaves the file on the ext2 side" '^ *hello-zenixfs$'
wantre "pwd inside the mounted filesystem" '^/mnt/dir2$'
wantre "data survives unmount and mount (wc again)" '^ *20000 +20000 +108894 /mnt/seq.txt$'
wantline "fsck.zenixfs reports clean" "fsck.zenixfs: clean"
# the host looks at the partition: find the superblock ('ZNFS' at byte 32 of its first block) on the scratch disk
python3 - <<'PY' > $OUT/offset.txt
import sys
d = open('build/scratch.hdf', 'rb').read()
off = 0
while True:
    i = d.find(b'ZNFS', off)
    if i < 0: break
    if i % 512 == 32 and d[i-28:i-26] == b'ZB':
        print(i - 32); break
    off = i + 1
PY
if [ -s $OUT/offset.txt ]; then
  start=$(cat $OUT/offset.txt)
  size=$(( 16*1024*1024 - start ))
  dd if=build/scratch.hdf of=$OUT/p2.img bs=512 skip=$(( start / 512 )) count=$(( size / 512 )) status=none
  zenixfs/build/zenixfs-tool fsck $OUT/p2.img > $OUT/hostfsck.log 2>&1 && check "host zenixfs-tool fsck: clean" ok || { check "host zenixfs-tool fsck: clean" bad; cat $OUT/hostfsck.log; }
  zenixfs/build/zenixfs-tool ls $OUT/p2.img / > $OUT/hostls.log 2>&1
  grep -q seq.txt $OUT/hostls.log && grep -q dir2 $OUT/hostls.log && check "host sees the files the Amiga wrote" ok || check "host sees the files the Amiga wrote" bad
  zenixfs/build/zenixfs-tool cat $OUT/p2.img /dir2/b.txt 2>/dev/null | grep -q hello-zenixfs && check "host reads a file the Amiga wrote" ok || check "host reads a file the Amiga wrote" bad
  seq 1 20000 | cmp -s - <(zenixfs/build/zenixfs-tool cat $OUT/p2.img /seq.txt) && check "host reads the 100 KB file byte for byte" ok || check "host reads the 100 KB file byte for byte" bad
else
  check "found the ZenixFS superblock on the scratch disk" bad
fi
exit $((1 - pass))
